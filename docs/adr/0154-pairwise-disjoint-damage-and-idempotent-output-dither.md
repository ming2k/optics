---
id: ADR-0154
title: "Pairwise-disjoint damage decomposition, single-pass output blits, and idempotent UNORM8 output dither"
status: accepted
date: 2026-11-09
scope: [flux]
superseded_by: null
negative_knowledge: true
---

# ADR-0154: Pairwise-disjoint damage decomposition, single-pass output blits, and idempotent UNORM8 output dither

- Status: Accepted
- Date: 2026-11-09
- Deciders: Optics Architecture Working Group
- Consulted: `libs/flux` (swapchain content sync, damage history, Vulkan canvas backend, colour management shaders)
- Amends: [ADR-0069](0069-color-management.md), [ADR-0143](0143-regional-damage-rect-lists-and-paint-bounds-localization.md), [ADR-0149](0149-damage-scoped-windowed-frames.md)

---

## Context and Problem Statement

ADR-0149 introduced damage-scoped Canvas passes and write-epoch swapchain repair
on windowed surfaces. Under real desktop composition (multi-band damage,
repeated `LOAD` passes, and multi-image swapchain rotation), four subtle GPU
contracts were violated simultaneously:

1. **Overlapping `VkImageCopy` regions in `flux_surface_sync_content` (`VUID-vkCmdCopyImage-pRegions-00124`)**:
   `flux_frame_record_written` appended every encoded rectangle from every pass
   in a frame, and `flux_damage_history_since` concatenated rectangles across
   all missed epochs `(since, committed]` without decomposing overlaps. Passing
   overlapping `VkImageCopy` regions into a single `vkCmdCopyImage` call is
   undefined behaviour under the Vulkan specification (`VUID-vkCmdCopyImage-pRegions-00124`);
   GPU copy engines and DCC/CCS tile metadata compressors race on the shared
   tiles, producing horizontal strip tearing and displaced blocks on windowed
   swapchains.
2. **Per-rectangle Dynamic Rendering passes in `record_output_blit`**:
   `vk_begin_pass` (the `LOAD` decode seed) and `vk_end_pass` (the output
   encode) opened and closed a separate `vkCmdBeginRendering` /
   `vkCmdEndRendering` pass for each damage rectangle with no inter-pass barrier
   and with `FLUX_LOAD_DONT_CARE` on decode. Whenever two damage rectangles
   overlapped or shared a GPU tile, the second rectangle's `LOAD_OP_DONT_CARE`
   clobbered the first rectangle's decoded intermediate pixels, and overlapping
   encode rectangles triggered Write-After-Write hazards on the destination.
3. **Non-idempotent 2-LSB TPDF output dither across `LOAD` passes**:
   `flux_dither_tpdf` added `(r + r2 - 1.0) / levels` $\in [-1/\text{levels}, +1/\text{levels}]$
   before hardware `UNORM8` rounding ($\lfloor y + 0.5 \rfloor$). Any pixel
   already at an exact 8-bit integer code value $k/255$ — such as a flat fill or
   an untouched background pixel inside a `LOAD` damage rectangle — has
   $|r + r_2 - 1.0| > 0.5$ on 25% of screen coordinates `(x, y)`. Every
   `out_decode -> RGBA16F -> out_encode` cycle therefore shifted those 25% of
   pixels by $\pm 1\text{ LSB}$ at fixed screen coordinates, accumulating visible
   static grain around high-frequency regional updates (such as a Dock or cursor
   band).
4. **Missing layout tracking on offscreen `begin_frame`**:
   `flux_surface_begin_frame` transitioned offscreen images to
   `VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL` without updating
   `s->image_layouts[image_index]`, leaving subsequent `LOAD`-pass barriers in
   `vk_begin_pass` with a stale `oldLayout`.

---

## Decision Outcome

### 1. Damage rectangles are decomposed into strictly pairwise-disjoint sets

- `flux_damage_history_commit` and `flux_damage_history_since` (`libs/flux/src/core/damage_history.c`)
  subtract every existing rectangle from each newly inserted rectangle (`c \ e`
  split into up to four non-overlapping bands: top, bottom, left, right). If the
  disjoint split exceeds capacity (`FLUX_DAMAGE_HISTORY_RECTS` or caller
  `capacity`), the history degrades conservatively to a full-extent copy.
- `flux_surface_sync_content` (`libs/flux/src/core/frame.c`) rounds float damage
  rectangles outward (`floorf` / `ceilf`) and decomposes the resulting integer
  boxes into strictly pairwise-disjoint `VkImageCopy` regions (falling back to a
  single full-surface `VkImageCopy` on capacity overflow), guaranteeing
  compliance with `VUID-vkCmdCopyImage-pRegions-00124`.
- `canvas_pass_areas` (`libs/flux/src/canvas/internal.h`) decomposes pass damage
  into strictly pairwise-disjoint `flux_recti` rectangles so no pixel is ever
  seeded or encoded twice within a pass.

### 2. Output decode and encode issue scissored draws inside a single pass

`record_output_blit` is replaced by `record_output_blits`
(`libs/flux/src/canvas/backend_vk.c`), which opens **one** Dynamic Rendering
pass over the union bounding box of `areas[0..area_count)` and records
`area_count` scissored fullscreen-triangle draws inside that single pass. This
eliminates `FLUX_LOAD_DONT_CARE` tile clobbering between adjacent/overlapping
damage rectangles during the `LOAD` seed and removes redundant render-pass
transitions during output encode.

### 3. `flux_dither_tpdf` is bounded inside the hardware UNORM rounding deadband

`flux_dither_tpdf` (`libs/flux/src/canvas/shaders/canvas_color.slang`) scales the
triangular PDF offset to `(r + r2 - 1.0) * (0.45 / levels)` ($\pm 0.45\text{ LSB}$).
Because $\pm 0.45\text{ LSB}$ plus the $\le 0.03\text{ LSB}$ `RGBA16F` working-space
round-trip tolerance remains strictly inside the $\pm 0.5\text{ LSB}$ hardware
`UNORM8` rounding interval around every integer code value $k \in \{0..255\}$,
`out_decode -> RGBA16F -> out_encode` is 100% bit-exact idempotent on all 256
8-bit code values across any number of `LOAD` passes, while fractional gradient
values between quantization steps continue to dither smoothly.

### 4. Offscreen `begin_frame` records `VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL` and windowed swapchains allocate `frames_in_flight + 1` images

- `flux_surface_begin_frame` updates `s->image_layouts[image_index] = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL`
  immediately after `barrier_to_color_attachment` on the offscreen path so
  subsequent `LOAD` pass barriers always observe the true image layout.
- `flux_surface_create_swapchain` (`libs/flux/src/core/surface.c`) ensures
  `image_count` is at least `s->frames_in_flight + 1u` (clamped by
  `caps.maxImageCount`) so `VK_PRESENT_MODE_MAILBOX_KHR` swapchains on Wayland
  WSI (`minImageCount == 2`, `frames_in_flight == 3`) allocate 4 images and
  never starve `vkAcquireNextImageKHR` while the outer compositor holds both the
  active scanout buffer and a newly committed buffer.

### 5. A Canvas pass owns its damage scope at `begin`, not the caller's array

`flux_canvas_pass_desc.damage` is read and copied into canvas-owned storage
during `flux_canvas_begin`. Every subsequent draw-scissor query and the
end-of-pass encode resolve the copy; the caller's rectangle array is free to be
reused, relocated, or freed as soon as `begin` returns. The same copy is made
for the CPU backend.

This closes a use-after-free that survived because it was invisible in C
examples and in full-extent passes. The Rust binding builds its rect list in a
temporary `Vec` (`CanvasPassOptions::damage`); after `begin` returned, the C
canvas held a dangling pointer. A damage-scoped pass then resolved a garbage
or out-of-bounds scope at encode time, emitted **zero** output blits, and left
the previous frame's pixels on screen. A moving window left its old body and
shadow behind; a closing window's ghost never disappeared; a scissored pass
could apply the wrong scissor to every draw. Full-extent passes were unaffected
(their scope is a null array), which made the corruption look like a
damage-tracking bug rather than a lifetime bug.

`test_partial_paint` now mutates and frees the caller's rect array immediately
after `begin` and asserts that the pass still writes its original band.

---

## Consequences

### Positive

- `vkCmdCopyImage` in `flux_surface_sync_content` never receives overlapping
  source or destination regions (`VUID-vkCmdCopyImage-pRegions-00124`).
- Multi-rectangle `LOAD` and encode steps execute in a single Dynamic Rendering
  pass with zero tile clobbering or Write-After-Write hazards.
- Repeated `LOAD` passes over an 8-bit surface produce zero LSB drift and zero
  spatial grain accumulation on untouched pixels (`test_color_management` case 14).
- A damage-scoped pass always encodes its scope. The pass API no longer depends
  on caller storage outliving a call it cannot observe.

### Negative

- Splitting partially overlapping rectangles can increase rectangle count by up
  to 3 rects per intersection; sets that exceed `FLUX_DAMAGE_CAPACITY` (64)
  degrade cleanly to a full-extent pass or copy.
