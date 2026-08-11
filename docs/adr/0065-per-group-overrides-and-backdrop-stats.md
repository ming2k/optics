# ADR-0065: Per-group material overrides and backdrop statistics

- Status: Accepted
- Date: 2026-08-12

## Context

The liquid-glass material (ADR-0046, moved to prism by ADR-0063) renders
every body in a dispatch with the same `frost_strength`, `tint_strength`,
and `saturation`: they are descriptor-wide knobs. A compositor laying out a
Dock, HUD chips, and a focused popover in one dispatch needs those knobs
per body, and it needs two measurements to drive them well — the mean
backdrop luminance behind a body (so the whole plate can take one uniform
adaptive-tint polarity instead of the per-pixel polarity, which can split a
body across a bright/dark boundary) and the backdrop's high-frequency
energy (so tint can strengthen over busy content). Two constraints shape
the design:

- **The push-constant budget is exactly 160 bytes** (flux guarantees that
  minimum on every device), and ADR-0050 already spent it to the byte. Any
  new per-group input must be funded by packing, not by growth.
- **prism must not learn policy.** ADR-0063's split exists so that flux
  owns mechanism, prism owns material identity, and the caller owns
  policy. Measuring backdrops is material-adjacent mechanism; deciding
  what a body should do with the numbers — including any temporal
  smoothing — is product policy.

## Decision

1. **Five per-group fields with negative sentinels** in
   `prism_liquid_glass_group` (`libs/prism/include/prism/liquid_glass.h`):
   `frost_strength` / `tint_strength` / `saturation` override the same
   descriptor knobs when `>= 0` and inherit them when `< 0`;
   `plate_polarity` (`[0,1]`) pins the caller-chosen adaptive-tint polarity
   for the whole body — 0 the smoke plate, 1 the pearl plate (`< 0` keeps
   the legacy per-pixel polarity);
   `backdrop_energy` (`[0,1]`) boosts the body tint by up to 2× over busy
   backdrops (`< 0` disables). `PRISM_LIQUID_GLASS_GROUP_INIT` defaults all
   five to `-1.0f`; groups must be built from the INIT macro because
   zero-init is an explicit zero override, not inherit. Validation
   (`liquid_glass_group_is_valid` in `libs/prism/src/regions.h`) requires
   all five to be finite and the two adaptive inputs to be `< 0` or within
   `[0, 1]`.
2. **The push block stays at exactly 160 bytes by repacking**
   (`libs/prism/src/liquid_glass.c`, `libs/prism/src/shaders/
   liquid_glass.comp`): `tint_color` (24 bits) and `shape_count` (2 bits)
   merge into one u32 (`tint_color | shape_count << 24`), and
   `size_reference` / `size_scale_min` pack as two IEEE-754 binary16 halves
   in one u32 (host-side round-to-nearest-even conversion, shader-side
   `unpackHalf2x16`; both are magnitudes where f16's ~3 decimal digits are
   ample). The freed 8 bytes carry `plate_polarity` and
   `backdrop_energy`. The three strength overrides flow through their
   existing push slots, resolved at fill time by
   `liquid_glass_group_or_desc` (negative → desc value).
3. **The material curves keep their shapes; only their inputs change**
   (ADR-0047's identity rule survives the ADR-0063 split): a non-negative
   `plate_polarity` replaces the per-pixel `dark_backdrop` sample outright
   (uniform per body, and chosen by the caller — the polarity decision moves
   out of the shader because the caller alone knows the text tone the plate
   must oppose); the frost curve decouples interior from
   rim — `0.08·strength + 0.20·u²·min(strength, 1)` — so a raised per-group
   strength fogs the interior while the rim band keeps the classic profile
   (refraction legibility at the silhouette); a non-negative
   `backdrop_energy` scales `tint_amount` by `min(1 + 1.5·energy, 2)`.
4. **Backdrop statistics are a GPU reduction with a frame-lagged CPU
   read** (`libs/prism/src/shaders/backdrop_stats.comp`):
   `prism_liquid_glass_filter_apply` dispatches one 16×16 workgroup per
   group before the glass loop, grid-striding the primary body's
   image-clipped bounds with an integer stride chosen so a group stays
   under ~64k samples. Each sample contributes the blurred input's Rec.709
   luma and |sharp − blurred| luma; a shared-memory reduction yields
   `vec2(mean_luminance, high_freq_energy)` per group, written through a
   buffer reference into a per-slot 512-byte HOST_VISIBLE `flux_buffer`
   (`FLUX_BUFFER_USAGE_STORAGE`, `device_address = true`). The address
   rides the push block as two u32s and is reconstructed with the
   `GL_EXT_buffer_reference_uvec2` constructor, because flux devices
   require `bufferDeviceAddress` (checked in `libs/flux/src/core/device.c`)
   but do not enable `shaderInt64`. flux's HOST_VISIBLE memory is
   HOST_COHERENT, so no flush/invalidate is needed; the slot fence waited
   by `begin_frame` makes the writes host-visible.
   `prism_liquid_glass_filter_stats` copies the stats out while the frame
   records — i.e. the values from `FLUX_MAX_FRAMES_IN_FLIGHT` frames ago —
   and reports the submitted group count honestly (a zero-group apply
   reports zero groups; a never-applied slot returns
   `FLUX_ERROR_INVALID_STATE`).
5. **Policy stays caller-side.** prism measures and exposes the two
   numbers; the caller decides the mapping onto `plate_polarity` /
   `backdrop_energy` / the strength overrides, and owns any temporal
   smoothing (the 3-frame lag makes unsmoothed feedback jitter).
   prism holds no frame-to-frame material state beyond the frame-slot
   buffers flux's model already provides.

## Alternatives Considered

- **Grow the push block past 160 bytes / move group data to a buffer.**
  flux guarantees 160 bytes, not more, so growth would fail on conformant
  flux devices; a per-group buffer would re-plumb descriptors or BDA reads
  into the hot material shader to carry 8 bytes of data. Packing the two
  lossless-enough pairs (24-bit tint + 2-bit count; f16 size magnitudes)
  is free and keeps the single-block dispatch idiom.
- **Store per-group overrides in a GPU buffer read by the glass shader.**
  Same rejection: the values are push-sized after the repack, and a buffer
  indirection per dispatch buys nothing.
- **CPU-side statistics.** Sampling the backdrop on the CPU would need a
  readback of both full images (sharp and blurred) per frame — bandwidth
  and latency the GPU reduction avoids with one workgroup per group and a
  512-byte persistent buffer.
- **Storage image + `vkCmdCopyImageToBuffer` fallback.** Unnecessary:
  flux devices are required to support `bufferDeviceAddress`, so the
  buffer-reference path always works; the uvec2 constructor variant
  (`GL_EXT_buffer_reference_uvec2`) removes the only feature gap
  (`shaderInt64`). No copy path is implemented.
- **Smoothing inside prism.** Rejected by the ADR-0063 boundary:
  temporal smoothing is a product decision (attack/decay curves are
  look-defining), and keeping prism stateless across frames beyond the
  frame-slot contract is what keeps the material testable and the
  boundary structural.

## Consequences

- **Breaking ABI change within prism:** `prism_liquid_glass_group` grows
  five floats. Source-compatible for callers building groups from
  `PRISM_LIQUID_GLASS_GROUP_INIT`; bare designated initializers silently
  change meaning (zero pins instead of inherits) — the in-tree examples
  were switched to the INIT macro, and the header documents the rule.
- The push-constant layout changes without changing size; the
  `static_assert(sizeof(liquid_glass_push) == 160)` guard and the matching
  GLSL block keep both sides in lockstep. f16 size parameters introduce a
  rounding of at most ~0.05% on size scaling — invisible for a floor and a
  reference magnitude.
- The stats dispatch adds one 16×16 workgroup per group per frame (≤ 64k
  samples each) and one 512-byte HOST_VISIBLE buffer per frame slot.
- `prism_liquid_glass_filter_stats` reads are 3 frames stale by
  construction; callers feeding the values back into the same bodies must
  smooth, or transient motion will oscillate.
- prism-sys' bindgen layout tests pin the new C structs; the safe `prism`
  crate exposes the fields as `Option<f32>` (`None` → `-1.0`) and the
  stats read as `LiquidGlassFilter::stats`.
- Docs: `docs/reference/prism.md` carries the group table, the stats
  contract, and the INIT-macro rule.

## References

- [ADR-0046 — Liquid glass as a convex-lens material](0046-liquid-glass-convex-lens-model.md)
- [ADR-0047 — Caller-owned policy boundary for flux effects](0047-caller-owned-policy-boundary-for-flux-effects.md)
- [ADR-0050 — Single-body liquid-glass focus field](0050-single-body-liquid-glass-focus-field.md)
- [ADR-0063 — Liquid glass moves to the prism material library](0063-liquid-glass-material-library.md)
- Vulkan `VK_KHR_buffer_device_address`; GLSL `GL_EXT_buffer_reference` and
  `GL_EXT_buffer_reference_uvec2`
