# ADR-0069: Color management — parametric color spaces, scRGB working space, explicit output transform

- Status: Accepted
- Date: 2026-08-15

## Context

Flux has no color management. Every module assumes "8-bit sRGB-ish":

- Canvas and scene shaders treat vertex colors, tints, gradients and
  sampled textures as raw values and blend them directly in gamma space
  on 8-bit `UNORM` attachments (`libs/flux/src/canvas/renderer.c`,
  `libs/flux/src/scene/shaders/scene_phong.frag`). Blending in gamma
  space is physically wrong and produces visible dark fringes.
- The swapchain is negotiated as `B8G8R8A8_UNORM` +
  `VK_COLOR_SPACE_SRGB_NONLINEAR_KHR` (`pick_format`,
  `libs/flux/src/core/surface.c`) — values written verbatim are
  *displayed* as sRGB, so shader output doubles as the transfer
  function by accident.
- `hdr_preferred` can select HDR10 / scRGB swapchains, but no shader
  performs PQ encoding or tone mapping, so HDR output is unusable in
  practice. `VK_EXT_swapchain_colorspace` is not even enabled, which
  is latent validation debt.
- Content has no color space tag: a Display P3 photo, an sRGB texture
  and a BT.2020 video frame are indistinguishable.
- There is no ICC support anywhere.

Downstream projects need real color management: wide-gamut content,
HDR output, and ICC-tagged media. Bolting these onto the current
"gamma everywhere" pipeline would produce three parallel partial
mechanisms — exactly the historical baggage we refuse to carry.

## Decision

Flux adopts a single color-managed architecture with five parts,
built in five independent milestones. One model — *primaries +
transfer function* — is carried through every layer.

**1. A parametric color space model is the single source of truth.**
A color space is `{ primaries, transfer function }`
(`flux_color_space` in `<flux/core.h>`): named primaries
(BT.709 / Display P3 / BT.2020 / Adobe RGB, all D65, plus custom
CIE xy) × named transfer functions (linear / sRGB / γ / PQ / HLG).
This is the model Vulkan (`VkColorSpaceKHR`), DXGI, the Wayland
color-management protocol and CSS Color 4 have all converged on.
Named presets cover the CSS Color 4 set (`FLUX_COLOR_SPACE_SRGB`,
`DISPLAY_P3`, `BT2020_PQ`, `SCRGB`, …). CPU-side math (transfer
encode/decode, primaries→XYZ matrices, Bradford adaptation,
source→destination transform matrices) lives in
`libs/flux/src/math/colorspace.c`. There is deliberately **no range
field**: flux renders RGB only; video-range YUV belongs to the
decoder that hands flux already-RGB content.

**2. The working space is fixed: extended linear BT.709 ("scRGB").**
All blending, gradients and lighting run in linear light on
`RGBA16_SFLOAT` intermediate images. sRGB content is the identity
transform; out-of-gamut colors ride as negative or >1 components and
are gamut-mapped only at output. The luminance scale follows scRGB:
1.0 = 80 cd/m², so SDR content needs no rescaling and scRGB
swapchains are pass-through. The working space is **not
configurable** — configurable working spaces are where partial color
pipelines go to die.

**3. Every render pass targeting a surface ends with an explicit
output transform.** Content decodes into the working space at the
edges (shader-side transfer decode + primaries matrix, or a hardware
`*_SRGB` view when the content is exactly sRGB — a fast path that is
a special case of the general path, not a parallel mechanism), and a
final full-screen transform pass converts working space → the
surface's actual color space, applying tone mapping (4) and dither
when quantizing to 8 bit. Scene and canvas share the same
intermediate-and-transform structure.

**4. HDR is explicit, outside any profile mechanism.** PQ (ST 2084)
and HLG are first-class transfer functions. SDR white in an HDR
surface defaults to 203 cd/m² (ITU-R BT.2408 graphics white) and is
configurable. Tone mapping is a pluggable stage of the output
transform shipping one default (BT.2390-style down-mapping; SDR→HDR
uses inverse-EOTF expansion). HDR static metadata
(`VK_EXT_hdr_metadata`) is a surface-level desc extension.

**5. ICC is a source of transforms, not a transform engine.** A
vendored, single-purpose ICC parser (skcms — MIT, two source files,
the same library Chrome/Skia use) parses v2/v4 profiles. Matrix+TRC
profiles are *extracted* into the parametric model and take the
analytic shader path; LUT profiles are evaluated once on the CPU at
load time and baked into a 65³ 3D-LUT texture that slots into the
same shader transform. Rendering intent defaults to relative
colorimetric (CSS canvas semantics); perceptual exists only inside
the ICC LUT bake. iccMAX, soft proofing and print separation are out
of scope. Display-profile discovery (colord / ICM / ColorSync) is a
separate, optional platform layer that merely *supplies* a target
`flux_color_space`; on Wayland with the color-management protocol the
compositor is the final transform and flux only tags.

**Surface negotiation becomes a priority list.** `flux_surface_desc`
gains a `next`-chain extension carrying an ordered array of
`flux_color_space`; flux intersects it with the swapchain's supported
(format, colorSpace) pairs and reports the winner via
`flux_surface_info.color_space`. `hdr_preferred` is retained as a
legacy alias mapping to `[BT2020_PQ, SCRGB, SRGB]` — its current
observable pick order is preserved. Offscreen surfaces default to
sRGB; the same extension can request a different offscreen space
(the format field ADR-0013 anticipated).

## Alternatives Considered

- **Full ICC CMM at the center (lcms2-style).** Rejected: CPU
  per-pixel transforms cannot run a real-time GPU pipeline; iccMAX
  (the only ICC generation that gestures at HDR) has essentially zero
  ecosystem support and lcms2 does not implement it. ICC becomes an
  input format, not the architecture.
- **Hardware `*_SRGB` formats as the whole story (status quo).**
  Rejected: covers exactly one transfer function, keeps blending in
  gamma space, and cannot express P3/BT.2020/HDR at all.
- **Configurable working space (e.g. optional ACEScg / AP1).**
  Rejected: doubles every shader's transform matrix count, breaks
  the identity fast path for sRGB content, and serves a film-VFX
  audience flux does not have. ACES tone-mapping *curves* remain
  available as pluggable output operators without moving the working
  space.
- **Convert everything at upload time on the CPU.** Rejected:
  destroys precision (8-bit sources stay 8-bit), makes gradients and
  lighting wrong, and cannot handle HDR or live video.
- **Wait for Wayland color-management and delegate everything to the
  compositor.** Rejected as a sole strategy: the protocol is not
  universal (Windows/macOS/X11 need the Vulkan path anyway), and
  compositor tagging still requires a correct internal pipeline to
  produce the tagged signal.

## Consequences

- New invariants: no shader may write a non-linear value to an
  intermediate attachment; no pass may target a surface without the
  output transform; 8-bit `UNORM` attachments are treated as
  transfer-encoded, never linear.
- `flux_surface_info` grows a `flux_color_space` field (pre-1.0,
  source-compatible for designated initializers; Rust bindgen
  regenerates).
- `VK_EXT_swapchain_colorspace` is enabled when advertised, fixing
  the latent validation debt for non-sRGB swapchains.
- Device auto-enable block grows `VK_EXT_hdr_metadata` (milestone 3).
- Effect pipeline storage formats must widen beyond RGBA8 UNORM or
  effects stay an SDR-only island (decided per milestone 2/3: effects
  accept 16F when the device advertises the format features).
- Documentation owed in the same commits: `docs/reference/symbols.md`
  rows, `docs/reference/glossary.md` terms (working space, transfer
  function, primaries, tone mapping, SDR white), and how-to updates.
- Golden-image tests (`test_canvas_consistency.c`,
  `test_scene_render.c`) change semantics when blending moves to
  linear light; baselines are re-recorded and new HDR/wide-gamut
  cases added via offscreen surface + readback.
- The CPU canvas backend (`backend_cpu.c`) must implement the same
  linear-light blending to remain the cross-platform oracle.

## References

- Vulkan: `VkColorSpaceKHR`, `VK_EXT_swapchain_colorspace`,
  `VK_EXT_hdr_metadata`
- ITU-R BT.2100 (PQ/HLG), BT.2390 (tone mapping), BT.2408 (SDR white
  203 cd/m²)
- ICC.1:2010 (v4) profile specification; skcms (Google, MIT)
- W3C CSS Color Module Level 4 (named spaces, relative colorimetric)
- Wayland color-management protocol (`wp_color_manager_v1`)
- ADR-0009 (canvas pipeline key), ADR-0013 (offscreen surface),
  ADR-0048 (materials and core images)
