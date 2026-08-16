# How to manage color

Color management in flux ([ADR-0069](../adr/0069-color-management.md),
[ADR-0070](../adr/0070-icc-in-tree-parser.md)) in one paragraph: every
canvas pass renders into an RGBA16F **working space** (extended linear
BT.709, "scRGB"; 1.0 = 80 cd/m²), content is decoded into it at the
edges, and an **output transform** converts to the surface's color
space at the end of the pass — with tone mapping and dither where
needed. Blending, gradients and lighting are always linear-light.

This guide is the practical tour: picking a surface space, drawing
wide-gamut and HDR content, tagging images, and ICC profiles.

## Prerequisites

Nothing to enable — the pipeline is always on. `flux_color` values are
sRGB (as always); the change is that they are now *converted* correctly
instead of passed through raw.

## Pick the surface's color space

Pass an ordered preference list; flux picks the first entry the
swapchain supports and reports the winner:

    flux_color_space want[] = {
        FLUX_COLOR_SPACE_DISPLAY_P3,   /* first choice */
        FLUX_COLOR_SPACE_SRGB,         /* fallback */
    };
    flux_surface_color_space_desc csd = FLUX_SURFACE_COLOR_SPACE_DESC_INIT;
    csd.spaces = want;
    csd.space_count = 2;

    flux_surface_desc sd = FLUX_SURFACE_DESC_INIT;
    sd.next = &csd;
    sd.vk_surface_khr = my_vk_surface;
    flux_surface *s = nullptr;
    if (flux_surface_create(d, &sd, &s) != FLUX_OK) return;

    flux_surface_info info;
    flux_surface_get_info(s, &info);
    /* info.color_space  — the negotiated swapchain space
     * info.content_space — the space pixels are written in */
    /* info.hdr — true for PQ / HLG / extended-linear */

Without the extension, behaviour is unchanged: `hdr_preferred = true`
maps to `[BT2020_PQ, SCRGB, SRGB]`, plain surfaces to `[SRGB]`.
Offscreen surfaces adopt the first listed space whose container the
device supports, the container following the transfer function: BGRA8
for sRGB/gamma (the historic default), RGB10A2 then RGBA16F for PQ/HLG,
RGBA16F for linear. Constrain the container explicitly with
`flux_surface_offscreen_format_desc` — a compositor exporting the
images through DRM/KMS uses it to match its plane formats, and reads
the winner back as `flux_surface_info.format` alongside the spaces.

You never re-encode anything yourself: draw as usual and the output
transform targets `content_space`.

## HDR output

Request an HDR space and tune presentation with the HDR desc:

    flux_color_space want[] = {
        FLUX_COLOR_SPACE_BT2020_PQ,    /* HDR10 */
        FLUX_COLOR_SPACE_SCRGB,        /* extended-linear fallback */
        FLUX_COLOR_SPACE_SRGB,
    };
    /* ... csd as above ... */

    flux_surface_hdr_desc hdr = FLUX_SURFACE_HDR_DESC_INIT;
    hdr.sdr_white_nits = 250.0f;       /* 0 = 203 (BT.2408 graphics white) */
    hdr.has_metadata = true;           /* optional HDR10 static metadata */
    hdr.mastering = (typeof(hdr.mastering)){ .rx = 0.708f, .ry = 0.292f,
        .gx = 0.170f, .gy = 0.797f, .bx = 0.131f, .by = 0.046f,
        .wx = 0.3127f, .wy = 0.3290f };
    hdr.max_luminance = 1000.0f;
    hdr.min_luminance = 0.0001f;
    hdr.max_cll = 1000.0f;
    hdr.max_fall = 400.0f;
    csd.next = &hdr;                   /* chain onto the surface desc */

Rules of the road:

- SDR content (everything in the 0..1 working range) is placed at
  `sdr_white_nits`; values above 1.0 are HDR headroom (working 2.0 ≈
  160 nits × the SDR-white scale).
- HDR10 static metadata is applied via `vkSetHdrMetadataEXT` when the
  device advertises `VK_EXT_hdr_metadata` (flux enables it
  automatically) — and reapplied on every swapchain recreation.
- HDR content drawn onto an SDR surface gets a hue-preserving shoulder
  rolloff; SDR-on-HDR is expanded as above.

## Tag image content

Untagged images keep the format default (8-bit = sRGB, 16F = linear).
Tag wide-gamut or otherwise non-default content:

    flux_color_space p3 = FLUX_COLOR_SPACE_DISPLAY_P3;
    flux_image_color_space_desc tag = FLUX_IMAGE_COLOR_SPACE_DESC_INIT;
    tag.space = &p3;

    flux_image_desc idesc = FLUX_IMAGE_DESC_INIT;
    idesc.next = &tag;
    idesc.width = w; idesc.height = h;
    idesc.format = FLUX_FORMAT_RGBA8_UNORM;
    idesc.initial_data = pixels;
    flux_image *img = nullptr;
    flux_image_create(d, &idesc, &img);

Tagging changes how texels are *decoded* into the working space; the
stored pixels are never rewritten.

## ICC profiles

Parse once (CPU only), then either query the parametric space or hand
the profile to an image:

    flux_icc_profile *icc = nullptr;
    if (flux_icc_profile_create(bytes, size, &icc) == FLUX_OK) {
        flux_color_space cs;
        if (flux_icc_profile_color_space(icc, &cs)) {
            /* matrix + TRC profile: usable anywhere a flux_color_space
             * is accepted — surface negotiation, image tags, math */
        } else {
            /* LUT profile: still fine on images — baked into a 3D LUT */
        }
    }

    tag.icc = icc;   /* takes precedence over tag.space */

Supported (ADR-0070): ICC v2/v4 display/scanner-class RGB profiles —
matrix+TRC (gamma or parametric 0–4 curves) and A2B0 LUTs
(mft1/mft2/mAB, tetrahedral CLUT, Lab PCS handled). Unsupported
profiles fail cleanly with `FLUX_ERROR_UNSUPPORTED`.

## Display profiles on legacy platforms

On color-managed compositors (Wayland color-management protocol,
Windows ACM) negotiate the true surface space and let the compositor
do the final transform. Where the compositor does *not* color-manage,
pre-convert into the display's actual space with the output override:

    flux_surface_output_color_desc out = FLUX_SURFACE_OUTPUT_COLOR_DESC_INIT;
    out.icc = display_icc;            /* must be parametric-extractable */
    /* or directly: out.content_space = (flux_color_space)FLUX_COLOR_SPACE_DISPLAY_P3; */

The swapchain then stays a plain sRGB container while pixels are
written in the display's space (`flux_surface_info.content_space`
reports it). Do not use this on managed platforms — you would
double-convert.

## Scene (3D) content

The scene module renders into whatever pass the caller sets up. The
color-managed pattern is render-to-texture then composite through the
canvas, which applies the output transform:

    /* once: a working-space target */
    flux_image *scene_rt = nullptr;
    flux_image_create_render_target(d, w, h, FLUX_FORMAT_RGBA16_SFLOAT, &scene_rt);

    /* per frame: render the scene into it, then composite */
    flux_frame_prepare_image_target(frame, scene_rt);
    /* begin pass on scene_rt->view, flux_scene_draw_mesh_* as usual */
    flux_frame_finish_image_target(frame, scene_rt);
    flux_canvas_draw_image(canvas, scene_rt, dst_rect, nullptr);

16F images are working-space linear, so the canvas treats them
correctly with no tag. Lights are already linear RGB
(`flux_scene_light`); mesh base colors are linear floats. When the
material's `color_format` is RGBA16_SFLOAT (as in the pattern above),
base-color *textures* go through the same decode rules as canvas
images — tagged via `flux_image_color_space_desc`, untagged 8-bit
UNORM sampled as sRGB — so lighting runs in linear light. Materials
declared for 8-bit targets keep the legacy raw-gamma path.

## Current boundaries

- The **effect** module (blur/backdrop, and the prism liquid-glass
  material built on it) follows its input: 8-bit SDR content keeps the
  historic RGBA8 path; 16F working-space inputs run in 16F (linear
  light) when the device advertises rgba16f storage, and fail cleanly
  with `FLUX_ERROR_UNSUPPORTED` where it does not.
- YUV is out of scope by design: video decoders hand flux RGB images
  with a color tag.
