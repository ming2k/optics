use optics_gfx::{
    AddressMode, Device, Filter, Format, Image, Material, MaterialAlphaMode, MaterialDesc,
    MaterialKind, MaterialOptions, MaterialTexture, Sampler, SamplerDesc,
};

use crate::glb_json::{AlphaMode, GlbDocument, GlbImage, GlbSampler};
use crate::{LoadError, MaterialTarget};

const MAX_TEXTURE_EDGE: u32 = 8192;
const MAX_IMAGE_ALLOCATION: u64 = 256 * 1024 * 1024;
const MAX_TOTAL_TEXTURE_BYTES: u64 = 512 * 1024 * 1024;

pub(crate) fn load(
    device: &Device,
    doc: &GlbDocument<'_>,
    target: MaterialTarget,
) -> Result<(Vec<Material>, Material), LoadError> {
    let mut required_textures = vec![false; doc.textures.len()];
    for material in &doc.materials {
        if let Some(info) = &material.base_color_texture {
            let tex = doc.textures.get(info.index).ok_or_else(|| {
                LoadError::Gltf(format!("texture index {} out of bounds", info.index))
            })?;
            let _ = tex;
            required_textures[info.index] = true;
        }
    }

    let mut images: Vec<Option<Image>> = std::iter::repeat_with(|| None)
        .take(doc.images.len())
        .collect();
    let mut samplers: Vec<Option<Sampler>> = std::iter::repeat_with(|| None)
        .take(doc.samplers.len())
        .collect();
    let mut default_sampler = None;
    let mut total_texture_bytes = 0u64;

    for (tex_index, texture) in doc.textures.iter().enumerate() {
        if !required_textures[tex_index] {
            continue;
        }
        let image_index = texture.source.ok_or_else(|| {
            LoadError::Unsupported(format!("texture {tex_index} has no source image"))
        })?;
        let image = doc
            .images
            .get(image_index)
            .ok_or_else(|| LoadError::Gltf(format!("image index {image_index} out of bounds")))?;
        if images[image_index].is_none() {
            let decoded = decode_image(device, doc, image)?;
            let (width, height) = decoded.size();
            let bytes = u64::from(width)
                .checked_mul(u64::from(height))
                .and_then(|pixels| pixels.checked_mul(4))
                .ok_or_else(|| LoadError::Unsupported("decoded image size overflows".into()))?;
            total_texture_bytes = total_texture_bytes
                .checked_add(bytes)
                .filter(|total| *total <= MAX_TOTAL_TEXTURE_BYTES)
                .ok_or_else(|| {
                    LoadError::Unsupported("decoded base-colour textures exceed 512 MiB".into())
                })?;
            images[image_index] = Some(decoded);
        }
        if let Some(sampler_index) = texture.sampler {
            let sampler = doc.samplers.get(sampler_index).ok_or_else(|| {
                LoadError::Gltf(format!("sampler index {sampler_index} out of bounds"))
            })?;
            if samplers[sampler_index].is_none() {
                samplers[sampler_index] = Some(Sampler::new(device, sampler_desc(sampler))?);
            }
        } else if default_sampler.is_none() {
            default_sampler = Some(Sampler::new(device, sampler_desc(&GlbSampler::default()))?);
        }
    }

    let mut materials = Vec::with_capacity(doc.materials.len());
    for (mat_index, material) in doc.materials.iter().enumerate() {
        let texture = if let Some(info) = &material.base_color_texture {
            let mut tex_coord = info.tex_coord;
            let mut uv_offset = [0.0, 0.0];
            let mut uv_scale = [1.0, 1.0];
            let mut uv_rotation = 0.0;
            if let Some(transform) = &info.transform {
                tex_coord = transform.tex_coord.unwrap_or(tex_coord);
                uv_offset = transform.offset;
                uv_scale = transform.scale;
                uv_rotation = transform.rotation;
            }
            if tex_coord != 0 {
                return Err(LoadError::Unsupported(format!(
                    "material {mat_index} uses TEXCOORD_{tex_coord}; only TEXCOORD_0 is uploaded"
                )));
            }
            let tex = &doc.textures[info.index];
            let image_index = tex
                .source
                .ok_or_else(|| LoadError::Unsupported("base-colour image was not loaded".into()))?;
            let image = images[image_index]
                .as_ref()
                .ok_or_else(|| LoadError::Unsupported("base-colour image was not loaded".into()))?;
            let sampler = match tex.sampler {
                Some(index) => samplers[index].as_ref(),
                None => default_sampler.as_ref(),
            };
            Some(MaterialTexture {
                image,
                sampler,
                uv_offset,
                uv_scale,
                uv_rotation,
            })
        } else {
            None
        };

        let alpha_mode = match material.alpha_mode {
            AlphaMode::Opaque => MaterialAlphaMode::Opaque,
            AlphaMode::Mask => MaterialAlphaMode::Mask,
            AlphaMode::Blend => MaterialAlphaMode::Blend,
        };
        materials.push(Material::new_with_options(
            device,
            MaterialDesc {
                kind: if material.unlit {
                    MaterialKind::Unlit
                } else {
                    MaterialKind::Phong
                },
                base_color: material.base_color_factor,
                color_format: target.color_format,
                depth_format: target.depth_format,
                shininess: 32.0,
                specular: 0.0,
            },
            MaterialOptions {
                base_color_texture: texture,
                alpha_mode,
                alpha_cutoff: material.alpha_cutoff,
                double_sided: material.double_sided,
            },
        )?);
    }

    let fallback = Material::new(
        device,
        MaterialDesc {
            kind: MaterialKind::Phong,
            base_color: [1.0; 4],
            color_format: target.color_format,
            depth_format: target.depth_format,
            shininess: 32.0,
            specular: 0.0,
        },
    )?;
    Ok((materials, fallback))
}

fn decode_image(
    device: &Device,
    doc: &GlbDocument<'_>,
    image: &GlbImage,
) -> Result<Image, LoadError> {
    if image.uri.is_some() {
        return Err(LoadError::Unsupported(
            "external image URIs are unsupported for in-memory GLB loading".into(),
        ));
    }
    let view_index = image
        .buffer_view
        .ok_or_else(|| LoadError::Unsupported("image has neither bufferView nor URI".into()))?;
    let view = doc
        .buffer_views
        .get(view_index)
        .ok_or_else(|| LoadError::Gltf(format!("bufferView index {view_index} out of bounds")))?;
    if view.buffer != 0 {
        return Err(LoadError::Unsupported(
            "base-colour image references an external buffer".into(),
        ));
    }
    let blob = doc
        .bin_chunk
        .ok_or_else(|| LoadError::Unsupported("GLB has no BIN chunk".into()))?;
    let end = view
        .byte_offset
        .checked_add(view.byte_length)
        .ok_or_else(|| LoadError::Unsupported("image buffer view overflows".into()))?;
    let encoded = blob
        .get(view.byte_offset..end)
        .ok_or_else(|| LoadError::Unsupported("image buffer view is out of bounds".into()))?;

    let mime = image
        .mime_type
        .as_deref()
        .ok_or_else(|| LoadError::Unsupported("image bufferView missing mimeType".into()))?;
    let (width, height, rgba) = match mime {
        "image/png" => {
            crate::image_codec::decode_png_rgba(encoded, MAX_TEXTURE_EDGE, MAX_IMAGE_ALLOCATION)
                .map_err(LoadError::Image)?
        }
        "image/jpeg" => {
            crate::image_codec::decode_jpeg_rgba(encoded, MAX_TEXTURE_EDGE, MAX_IMAGE_ALLOCATION)
                .map_err(LoadError::Image)?
        }
        other => {
            return Err(LoadError::Unsupported(format!(
                "unsupported base-colour image MIME type {other}"
            )));
        }
    };

    Image::from_bytes(device, width, height, Format::Rgba8Srgb, &rgba).map_err(LoadError::from)
}

fn sampler_desc(sampler: &GlbSampler) -> SamplerDesc {
    let (min_filter, mipmap_mode) = match sampler.min_filter {
        None | Some(9729) => (Filter::Linear, Filter::Linear),
        Some(9728) => (Filter::Nearest, Filter::Nearest),
        Some(9984) => (Filter::Nearest, Filter::Nearest),
        Some(9985) => (Filter::Linear, Filter::Nearest),
        Some(9986) => (Filter::Nearest, Filter::Linear),
        Some(9987) => (Filter::Linear, Filter::Linear),
        Some(_) => (Filter::Linear, Filter::Linear),
    };
    SamplerDesc {
        min_filter,
        mag_filter: match sampler.mag_filter {
            Some(9728) => Filter::Nearest,
            None | Some(9729) | Some(_) => Filter::Linear,
        },
        mipmap_mode,
        address_u: address_mode(sampler.wrap_s),
        address_v: address_mode(sampler.wrap_t),
        address_w: AddressMode::Repeat,
        max_anisotropy: 1.0,
    }
}

fn address_mode(mode: u32) -> AddressMode {
    match mode {
        33071 => AddressMode::ClampToEdge,
        33648 => AddressMode::MirroredRepeat,
        _ => AddressMode::Repeat,
    }
}
