use std::io::Cursor;

use flux::{
    AddressMode, Device, Filter, Format, Image, Material, MaterialAlphaMode, MaterialDesc,
    MaterialKind, MaterialOptions, MaterialTexture, Sampler, SamplerDesc,
};
use gltf::texture::{MagFilter, MinFilter, WrappingMode};
use image::{ImageFormat, ImageReader, Limits};

use crate::{LoadError, MaterialTarget};

const MAX_TEXTURE_EDGE: u32 = 8192;
const MAX_IMAGE_ALLOCATION: u64 = 256 * 1024 * 1024;
const MAX_TOTAL_TEXTURE_BYTES: u64 = 512 * 1024 * 1024;

pub(crate) fn load(
    device: &Device,
    gltf: &gltf::Gltf,
    target: MaterialTarget,
) -> Result<(Vec<Material>, Material), LoadError> {
    let document = &gltf.document;
    let mut required_textures = vec![false; document.textures().count()];
    for material in document.materials() {
        if let Some(info) = material.pbr_metallic_roughness().base_color_texture() {
            required_textures[info.texture().index()] = true;
        }
    }

    let mut images: Vec<Option<Image>> = std::iter::repeat_with(|| None)
        .take(document.images().count())
        .collect();
    let mut samplers: Vec<Option<Sampler>> = std::iter::repeat_with(|| None)
        .take(document.samplers().count())
        .collect();
    let mut default_sampler = None;
    let mut total_texture_bytes = 0u64;

    for texture in document.textures() {
        if !required_textures[texture.index()] {
            continue;
        }
        let image = texture.source();
        let image_index = image.index();
        if images[image_index].is_none() {
            let decoded = decode_image(device, gltf, &image)?;
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
        let sampler = texture.sampler();
        if let Some(index) = sampler.index() {
            if samplers[index].is_none() {
                samplers[index] = Some(Sampler::new(device, sampler_desc(&sampler))?);
            }
        } else if default_sampler.is_none() {
            default_sampler = Some(Sampler::new(device, sampler_desc(&sampler))?);
        }
    }

    let mut materials = Vec::with_capacity(document.materials().count());
    for material in document.materials() {
        let pbr = material.pbr_metallic_roughness();
        let texture = if let Some(info) = pbr.base_color_texture() {
            let mut tex_coord = info.tex_coord();
            let mut uv_offset = [0.0, 0.0];
            let mut uv_scale = [1.0, 1.0];
            let mut uv_rotation = 0.0;
            if let Some(transform) = info.texture_transform() {
                tex_coord = transform.tex_coord().unwrap_or(tex_coord);
                uv_offset = transform.offset();
                uv_scale = transform.scale();
                uv_rotation = transform.rotation();
            }
            if tex_coord != 0 {
                return Err(LoadError::Unsupported(format!(
                    "material {} uses TEXCOORD_{tex_coord}; only TEXCOORD_0 is uploaded",
                    material.index().unwrap_or(0)
                )));
            }
            let texture = info.texture();
            let image = images[texture.source().index()]
                .as_ref()
                .ok_or_else(|| LoadError::Unsupported("base-colour image was not loaded".into()))?;
            let sampler_info = texture.sampler();
            let sampler = match sampler_info.index() {
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

        let alpha_mode = match material.alpha_mode() {
            gltf::material::AlphaMode::Opaque => MaterialAlphaMode::Opaque,
            gltf::material::AlphaMode::Mask => MaterialAlphaMode::Mask,
            gltf::material::AlphaMode::Blend => MaterialAlphaMode::Blend,
        };
        materials.push(Material::new_with_options(
            device,
            MaterialDesc {
                kind: if material.unlit() {
                    MaterialKind::Unlit
                } else {
                    MaterialKind::Phong
                },
                base_color: pbr.base_color_factor(),
                color_format: target.color_format,
                depth_format: target.depth_format,
                shininess: 32.0,
                specular: 0.0,
            },
            MaterialOptions {
                base_color_texture: texture,
                alpha_mode,
                alpha_cutoff: material.alpha_cutoff().unwrap_or(0.5),
                double_sided: material.double_sided(),
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
    gltf: &gltf::Gltf,
    image: &gltf::image::Image<'_>,
) -> Result<Image, LoadError> {
    let (encoded, format) = match image.source() {
        gltf::image::Source::View { view, mime_type } => {
            if !matches!(view.buffer().source(), gltf::buffer::Source::Bin) {
                return Err(LoadError::Unsupported(
                    "base-colour image references an external buffer".into(),
                ));
            }
            let blob = gltf
                .blob
                .as_deref()
                .ok_or_else(|| LoadError::Unsupported("GLB has no BIN chunk".into()))?;
            let end = view
                .offset()
                .checked_add(view.length())
                .ok_or_else(|| LoadError::Unsupported("image buffer view overflows".into()))?;
            let encoded = blob.get(view.offset()..end).ok_or_else(|| {
                LoadError::Unsupported("image buffer view is out of bounds".into())
            })?;
            (encoded, image_format(mime_type)?)
        }
        gltf::image::Source::Uri { .. } => {
            return Err(LoadError::Unsupported(
                "external image URIs are unsupported for in-memory GLB loading".into(),
            ));
        }
    };

    let mut reader = ImageReader::with_format(Cursor::new(encoded), format);
    let mut limits = Limits::default();
    limits.max_image_width = Some(MAX_TEXTURE_EDGE);
    limits.max_image_height = Some(MAX_TEXTURE_EDGE);
    limits.max_alloc = Some(MAX_IMAGE_ALLOCATION);
    reader.limits(limits);
    let rgba = reader.decode()?.into_rgba8();
    Image::from_bytes(
        device,
        rgba.width(),
        rgba.height(),
        Format::FLUX_FORMAT_RGBA8_SRGB,
        rgba.as_raw(),
    )
    .map_err(LoadError::from)
}

fn image_format(mime_type: &str) -> Result<ImageFormat, LoadError> {
    match mime_type {
        "image/png" => Ok(ImageFormat::Png),
        "image/jpeg" => Ok(ImageFormat::Jpeg),
        other => Err(LoadError::Unsupported(format!(
            "unsupported base-colour image MIME type {other}"
        ))),
    }
}

fn sampler_desc(sampler: &gltf::texture::Sampler<'_>) -> SamplerDesc {
    let (min_filter, mipmap_mode) = match sampler.min_filter() {
        None | Some(MinFilter::Linear) => (Filter::Linear, Filter::Linear),
        Some(MinFilter::Nearest) => (Filter::Nearest, Filter::Nearest),
        Some(MinFilter::NearestMipmapNearest) => (Filter::Nearest, Filter::Nearest),
        Some(MinFilter::LinearMipmapNearest) => (Filter::Linear, Filter::Nearest),
        Some(MinFilter::NearestMipmapLinear) => (Filter::Nearest, Filter::Linear),
        Some(MinFilter::LinearMipmapLinear) => (Filter::Linear, Filter::Linear),
    };
    SamplerDesc {
        min_filter,
        mag_filter: match sampler.mag_filter() {
            Some(MagFilter::Nearest) => Filter::Nearest,
            None | Some(MagFilter::Linear) => Filter::Linear,
        },
        mipmap_mode,
        address_u: address_mode(sampler.wrap_s()),
        address_v: address_mode(sampler.wrap_t()),
        address_w: AddressMode::Repeat,
        max_anisotropy: 1.0,
    }
}

fn address_mode(mode: WrappingMode) -> AddressMode {
    match mode {
        WrappingMode::ClampToEdge => AddressMode::ClampToEdge,
        WrappingMode::MirroredRepeat => AddressMode::MirroredRepeat,
        WrappingMode::Repeat => AddressMode::Repeat,
    }
}
