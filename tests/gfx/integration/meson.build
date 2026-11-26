# Integration tests: require a Vulkan ICD (lavapipe on headless CI, real
# drivers on developer hosts). A test whose environment is absent reports a
# Meson SKIP (exit 77) through OT_SKIP, never a silent PASS. The shared SPIR-V
# custom targets come from tests/gfx/meson.build via subdir() scope.

test_device = executable('test_device',
  'test_device.c',
  dependencies : [optics_gfx_test_support_dep],
)
# device_create/release touches the Vulkan loader, so this is an integration
# test even though it is light. It skips when no device can be created.
test('device', test_device, suite : 'integration')

# Observation-plane ledger (ADR-0129): unconditional counting, peak
# surviving drain, unmatched-release rejection, bounded interning that
# reports its own truncation, and the report's structured return shape.
# The ledger rows are device-scoped, so this binary belongs to the boundary
# layer even though its assertions are deterministic.
test_observe = executable('test_observe',
  'test_observe.c',
  dependencies : [optics_gfx_test_support_dep],
)
test('observe', test_observe, suite : 'integration')

test_buffer = executable('test_buffer',
  'test_buffer.c',
  dependencies : [optics_gfx_test_support_dep],
)
test('buffer', test_buffer, suite : 'integration')

# Buffer/target retire queue: churn create -> draw -> release every
# frame so inline destruction of an in-flight vertex buffer or depth
# target (the same i915 GPU-hang hazard as image_retire) fails loudly.
test_buffer_retire = executable('test_buffer_retire',
  'test_buffer_retire.c',
  triangle_attr_vert_spv, triangle_frag_spv,
  dependencies : [optics_gfx_test_support_dep],
)
test('buffer_retire', test_buffer_retire, suite : 'integration')

test_bindless = executable('test_bindless',
  'test_bindless.c',
  dependencies : [optics_gfx_test_support_dep],
)
test('bindless', test_bindless, suite : 'integration')

test_sampler = executable('test_sampler',
  'test_sampler.c',
  dependencies : [optics_gfx_test_support_dep],
)
test('sampler', test_sampler, suite : 'integration')

test_image_update = executable('test_image_update',
  'test_image_update.c',
  dependencies : [optics_gfx_test_support_dep],
)
test('image_update', test_image_update, suite : 'integration')

test_render_plan_frame = executable('test_render_plan_frame',
  'test_render_plan_frame.c',
  dependencies : [optics_gfx_test_support_dep],
)
test('render_plan_frame', test_render_plan_frame, suite : 'integration')

test_plan_effect_order = executable('test_plan_effect_order',
  'test_plan_effect_order.c',
  dependencies : [optics_gfx_test_support_dep],
)
test('plan_effect_order', test_plan_effect_order, suite : 'integration')

if host_machine.system() == 'linux'
  test_dmabuf = executable('test_dmabuf',
    'test_dmabuf.c',
    dependencies : [optics_gfx_test_support_dep],
  )
  test('dmabuf', test_dmabuf, suite : 'integration')
endif

test_compute = executable('test_compute',
  'test_compute.c',
  dependencies : [optics_gfx_test_support_dep],
)
test('compute', test_compute, suite : 'integration')

test_graphics_pipeline = executable('test_graphics_pipeline',
  'test_graphics_pipeline.c',
  triangle_vert_spv, triangle_frag_spv,
  dependencies : [optics_gfx_test_support_dep],
)
test('graphics_pipeline', test_graphics_pipeline, suite : 'integration')

test_allocator = executable('test_allocator',
  'test_allocator.c',
  dependencies : [optics_gfx_test_support_dep],
)
test('allocator', test_allocator, suite : 'integration')

# Resource leak lane (ADR-0130, INV-ARCH-LEDGER): one exact-delta assertion
# per public resource kind against the observation ledger, no tolerance
# constants, plus the bindless double-release invariant (ADR-0131) and a
# positive control that the ledger actually sees a deliberate leak.
test_resource_leak = executable('test_resource_leak',
  'test_resource_leak.c',
  triangle_attr_vert_spv, triangle_frag_spv,
  dependencies : [optics_gfx_test_support_dep],
)
test('resource_leak', test_resource_leak, suite : 'integration')
# Pass-label emission (ADR-0133). The capability was marked Landed with no
# assertion that a label ever reached the driver; this counts them.
test_pass_labels = executable('test_pass_labels',
  'test_pass_labels.c',
  dependencies : [optics_gfx_test_support_dep],
)
test('pass_labels', test_pass_labels, suite : 'integration')
# Hostile-allocator contract test (ADR-0135): the `core.h` rule that peer code
# must allocate through the device was never verified. This installs an
# allocator that sees everything and requires ui to pass through it.
if opt_ui
  test_hostile_allocator = executable('test_hostile_allocator',
    'test_hostile_allocator.c',
    dependencies : [optics_gfx_test_support_dep, ui_dep],
  )
  test('hostile_allocator', test_hostile_allocator, suite : 'integration')
endif



# VK_EXT_memory_budget: enabled-when-advertised, per-heap usage/budget
# must be sane on supporting drivers and zeroed cleanly elsewhere.
test_memory_budget = executable('test_memory_budget',
  'test_memory_budget.c',
  dependencies : [optics_gfx_test_support_dep],
)
test('memory_budget', test_memory_budget, suite : 'integration')

# OOM fault injection: interposed vkAllocateMemory drives the pool,
# dedicated, and reclaim-retry failure paths end to end. The interposer
# uses dlsym(RTLD_NEXT), so this lane is Linux-only.
if host_machine.system() == 'linux'
  test_alloc_fault = executable('test_alloc_fault',
    'test_alloc_fault.c',
    dependencies : [optics_gfx_test_support_dep, dl_dep],
  )
  test('alloc_fault', test_alloc_fault, suite : 'integration')
endif

# Batched uploads: one submission for a bulk load, auto-flush at frame
# begin, steady-state allocator accounting afterwards.
test_upload_batch = executable('test_upload_batch',
  'test_upload_batch.c',
  dependencies : [optics_gfx_test_support_dep],
)
test('upload_batch', test_upload_batch, suite : 'integration')

# SDF derivative parity (ADR-0148 / INV-SDF-02): dispatch the real
# optics_gfx_sdf.slang and compare distance + normal against <optics/gfx/sdf.h>.
sdf_parity_spv = custom_target('test_sdf_parity.comp.spv',
  input   : 'shaders/sdf_parity.comp.slang',
  depend_files : gfx_sdf_slang,
  output  : 'sdf_parity.comp.spv',
  command : [slangc, gfx_sdf_inc, '-target', 'spirv', '-profile', 'spirv_1_6', '-entry', 'main', '-stage', 'compute', '@INPUT@', '-o', '@OUTPUT@'],
)
test_sdf_parity = executable('test_sdf_parity',
  'test_sdf_parity.c',
  sdf_parity_spv,
  dependencies : [optics_gfx_test_support_dep],
)
test('sdf_parity', test_sdf_parity, suite : 'integration')


# GPU integration: needs a compiled compute shader.
test_compute_golden = executable('test_compute_golden',
  'test_compute_golden.c',
  fill_pattern_spv,
  dependencies : [optics_gfx_test_support_dep],
)
test('compute_golden', test_compute_golden, suite : 'integration')

test_validation = executable('test_validation',
  'test_validation.c',
  fill_pattern_spv,
  dependencies : [optics_gfx_test_support_dep],
)
test('validation', test_validation, suite : 'integration', timeout : 30)

test_soak = executable('test_soak',
  'test_soak.c',
  triangle_vert_spv, triangle_frag_spv,
  dependencies : [optics_gfx_test_support_dep],
)
test('soak', test_soak, suite : 'integration', timeout : 60)

# Offscreen surface + canvas pixel assertions (ADR-0013). Needs the
# canvas module for the drawing side of the round trip.
test_offscreen = executable('test_offscreen',
  'test_offscreen.c',
  dependencies : [optics_gfx_test_support_dep],
)
test('offscreen', test_offscreen, suite : 'integration')

# Native frame damage ledger: epoch history, first-use Full, abandoned
# submission retry, and scanout delta. This is the contract the compositor's
# area-damage pipeline relies on; it must not drift back into a caller-side
# per-image ledger.
test_frame_damage = executable('test_frame_damage',
  'test_frame_damage.c',
  dependencies : [optics_gfx_test_support_dep],
)
test('frame_damage', test_frame_damage, suite : 'integration')

if opt_text
  # Canvas feature matrix on the GPU: gradients, strokes, and the
  # hole-bridging fill (ADR-0011), asserted by read-back pixel.
  test_canvas_render = executable('test_canvas_render',
    'test_canvas_render.c',
    dependencies : [optics_gfx_test_support_dep, text_dep],
  )
  test('canvas_render', test_canvas_render, suite : 'integration')
endif

# Cross-platform consistency gate: one fixed scene rendered by the GPU
# backend and the CPU oracle (ADR-0019), compared pixel-by-pixel. This
# is the test that proves a Windows/macOS port renders identically.
test_canvas_consistency = executable('test_canvas_consistency',
  'test_canvas_consistency.c',
  dependencies : [optics_gfx_test_support_dep],
)
test('canvas_consistency', test_canvas_consistency, suite : 'integration')

# Coverage ceiling (ADR-0119 S3 target): measures the distinct coverage levels
# along one oblique edge on the GPU path (4x MSAA) against the CPU coverage
# path, so S3's motivation is a number in the suite and its completion inverts
# the assertion rather than going unnoticed.
test_canvas_coverage_ceiling = executable('test_canvas_coverage_ceiling',
  'test_canvas_coverage_ceiling.c',
  dependencies : [optics_gfx_test_support_dep],
)
test('canvas_coverage_ceiling', test_canvas_coverage_ceiling, suite : 'integration')

# The same contract for every draw class, not only the solid source-over fill:
# strokes and other blend modes reach the rasteriser by their own routes.
test_canvas_edge_quality = executable('test_canvas_edge_quality',
  'test_canvas_edge_quality.c',
  dependencies : [optics_gfx_test_support_dep],
)
test('canvas_edge_quality', test_canvas_edge_quality, suite : 'integration')

# Real-driver hardware assertion (ADR-0120 Phase B0 gate)
test_device_hardware = executable('test_device_hardware',
  'test_device_hardware.c',
  dependencies : [optics_gfx_test_support_dep],
)
test('device_hardware', test_device_hardware, suite : 'integration')

# Tile-compute coverage backend (ADR-0120 Phase B1 gate)
test_tile_b1_upload = executable('test_tile_b1_upload',
  'test_tile_b1_upload.c',
  '../../../libs/gfx/src/canvas/canvas_tile.c',
  '../../../libs/gfx/src/canvas/curve_flatten.c',
  dependencies : [optics_gfx_test_support_dep],
)
test('tile_b1_upload', test_tile_b1_upload, suite : 'integration')

# Tile-compute coarse raster binning (ADR-0120 Phase B2 gate)
test_tile_b2_coarse = executable('test_tile_b2_coarse',
  'test_tile_b2_coarse.c',
  '../../../libs/gfx/src/canvas/canvas_tile.c',
  dependencies : [optics_gfx_test_support_dep],
)
test('tile_b2_coarse', test_tile_b2_coarse, suite : 'integration')

# Tile-compute winding backdrop (ADR-0120 Phase B3 gate)
test_tile_b3_backdrop = executable('test_tile_b3_backdrop',
  'test_tile_b3_backdrop.c',
  '../../../libs/gfx/src/canvas/canvas_tile.c',
  dependencies : [optics_gfx_test_support_dep],
)
test('tile_b3_backdrop', test_tile_b3_backdrop, suite : 'integration')

# Tile-compute fine raster (ADR-0120 Phase B4 gate)
test_tile_b4_fine = executable('test_tile_b4_fine',
  'test_tile_b4_fine.c',
  '../../../libs/gfx/src/canvas/canvas_tile.c',
  '../../../libs/gfx/src/canvas/coverage_raster.c',
  dependencies : [optics_gfx_test_support_dep],
)
test('tile_b4_fine', test_tile_b4_fine, suite : 'integration')

# Deferred pass plan: pruning, materialization, and the in-pass contract
# (ADR-0147).
test_plan_prune = executable('test_plan_prune',
  'test_plan_prune.c',
  dependencies : [optics_gfx_test_support_dep],
)
test('plan_prune', test_plan_prune, suite : 'integration')

# Tile-compute blend and clip integration (ADR-0120 Phase B5 gate)
test_tile_b5_renderplan = executable('test_tile_b5_renderplan',
  'test_tile_b5_renderplan.c',
  '../../../libs/gfx/src/canvas/canvas_tile.c',
  '../../../libs/gfx/src/canvas/curve_flatten.c',
  dependencies : [optics_gfx_test_support_dep],
)
test('tile_b5_renderplan', test_tile_b5_renderplan, suite : 'integration')

# Real-driver GPU Tile-Compute rasterizer execution (Phase B hardware execution)
test_tile_gpu_compute = executable('test_tile_gpu_compute',
  'test_tile_gpu_compute.c',
  '../../../libs/gfx/src/canvas/canvas_tile.c',
  canvas_tile_raster_comp_spv,
  dependencies : [optics_gfx_test_support_dep],
)
test('tile_gpu_compute', test_tile_gpu_compute, suite : 'integration')

# Colour management (ADR-0069): linear-light blending, P3 output
# transform, LOAD seeding, and linear gradient interpolation, asserted
# by offscreen readback against the public colour math API.
test_color_management = executable('test_color_management',
  'test_color_management.c',
  dependencies : [optics_gfx_test_support_dep],
)
test('color_management', test_color_management, suite : 'integration')

# Partial (damage-rect) passes across a rotating image set (ADR-0144): a
# sub-rect LOAD pass may only composite onto an image known to hold a complete
# frame, and the other slots must not present a stale one.
test_partial_paint = executable('test_partial_paint',
  'test_partial_paint.c',
  dependencies : [optics_gfx_test_support_dep],
)
test('partial_paint', test_partial_paint, suite : 'integration')

# Image retire queue: churn create -> draw -> release every frame so
# inline destruction of in-flight images (GPU hang on i915) or a
# missing bindless-slot recycle fails loudly.
test_image_retire = executable('test_image_retire',
  'test_image_retire.c',
  dependencies : [optics_gfx_test_support_dep],
)
test('image_retire', test_image_retire, suite : 'integration')

if opt_text
  # Glyph-atlas image governance across atlas_clear: the retire loop once
  # guarded page 0's release with `p > 0`, leaking a 16 MiB dedicated
  # VkDeviceMemory per clear. Forces clears and asserts live allocations
  # return to baseline.
  test_text_atlas_leak = executable('test_text_atlas_leak',
    'test_text_atlas_leak.c',
    dependencies : [optics_gfx_test_support_dep, text_dep],
  )
  test('text_atlas_leak', test_text_atlas_leak,
    suite       : 'integration',
    is_parallel : false,
  )
endif

# Retire-FIFO backpressure: releasing past the pending bound with a
# frozen watermark must force a full drain, not unbounded growth.
test_retire_backpressure = executable('test_retire_backpressure',
  'test_retire_backpressure.c',
  dependencies : [optics_gfx_test_support_dep],
)
test('retire_backpressure', test_retire_backpressure,
  suite       : 'integration',
  is_parallel : false,
)

# Built only when the effect module is enabled.
test_effect_blur = executable('test_effect_blur',
  'test_effect_blur.c',
  dependencies : [optics_gfx_test_support_dep],
)
test('effect_blur', test_effect_blur, suite : 'integration')

# ADR-0205: the transient pool is bounded by bytes, not entry count.
test_effect_pool = executable('test_effect_pool',
  'test_effect_pool.c',
  dependencies : [optics_gfx_test_support_dep],
)
test('effect_pool', test_effect_pool, suite : 'integration')

# ADR-0074: drop-shadow operator.
test_effect_shadow = executable('test_effect_shadow',
  'test_effect_shadow.c',
  dependencies : [optics_gfx_test_support_dep],
)
test('effect_shadow', test_effect_shadow, suite : 'integration')

# Render-target capture (ADR-0017): capture -> blur -> composite.
test_canvas_target = executable('test_canvas_target',
  'test_canvas_target.c',
  dependencies : [optics_gfx_test_support_dep],
)
test('canvas_target', test_canvas_target, suite : 'integration')

# Bucketed target-attachment pool: a size sweep must not churn allocations.
test_canvas_target_pool = executable('test_canvas_target_pool',
  'test_canvas_target_pool.c',
  dependencies : [optics_gfx_test_support_dep],
)
test('canvas_target_pool', test_canvas_target_pool, suite : 'integration')

test_image_update_strided = executable('test_image_update_strided',
  'test_image_update_strided.c',
  dependencies : [optics_gfx_test_support_dep],
)
test('image_update_strided', test_image_update_strided, suite : 'integration')
