# Integration tests: require a Vulkan ICD (lavapipe on headless CI,
# real drivers on developer hosts). Each test gracefully skips with
# PASS when its required environment is absent — no need for a
# separate gating mechanism. The shared SPIR-V custom_targets come
# from the parent tests/meson.build via subdir() scope.

test_device = executable('test_device',
  'test_device.c',
  dependencies : [test_helpers_dep],
)
# device_create/release is pure ABI plumbing; group it with the
# other unit-scope checks so the unit suite stays green on hosts
# that happen to have an ICD but cannot run the heavier integration
# tests (e.g. lavapipe-only CI nodes).
test('device', test_device, suite : 'unit')

test_buffer = executable('test_buffer',
  'test_buffer.c',
  dependencies : [test_helpers_dep],
)
test('buffer', test_buffer, suite : 'integration')

# Buffer/target retire queue: churn create -> draw -> release every
# frame so inline destruction of an in-flight vertex buffer or depth
# target (the same i915 GPU-hang hazard as image_retire) fails loudly.
test_buffer_retire = executable('test_buffer_retire',
  'test_buffer_retire.c',
  triangle_attr_vert_spv, triangle_frag_spv,
  dependencies : [test_helpers_dep],
  c_args       : ['--embed-dir=' + core_tests_build_dir],
)
test('buffer_retire', test_buffer_retire, suite : 'integration')

test_bindless = executable('test_bindless',
  'test_bindless.c',
  dependencies : [test_helpers_dep],
)
test('bindless', test_bindless, suite : 'integration')

test_sampler = executable('test_sampler',
  'test_sampler.c',
  dependencies : [test_helpers_dep],
)
test('sampler', test_sampler, suite : 'integration')

test_image_update = executable('test_image_update',
  'test_image_update.c',
  dependencies : [test_helpers_dep],
)
test('image_update', test_image_update, suite : 'integration')

test_dmabuf = executable('test_dmabuf',
  'test_dmabuf.c',
  dependencies : [test_helpers_dep],
)
test('dmabuf', test_dmabuf, suite : 'integration')

test_compute = executable('test_compute',
  'test_compute.c',
  dependencies : [test_helpers_dep],
)
test('compute', test_compute, suite : 'integration')

test_graphics_pipeline = executable('test_graphics_pipeline',
  'test_graphics_pipeline.c',
  triangle_vert_spv, triangle_frag_spv,
  dependencies : [test_helpers_dep],
  c_args       : ['--embed-dir=' + core_tests_build_dir],
)
test('graphics_pipeline', test_graphics_pipeline, suite : 'integration')

test_scene_mesh = executable('test_scene_mesh',
  'test_scene_mesh.c',
  dependencies : [test_helpers_dep],
)
test('scene_mesh', test_scene_mesh, suite : 'integration')

# Mesh retire queue: churn create -> draw -> release every frame so
# inline destruction of in-flight vertex/index buffers fails loudly.
if opt_scene
  test_mesh_retire = executable('test_mesh_retire',
    'test_mesh_retire.c',
    dependencies : [test_helpers_dep],
  )
  test('mesh_retire', test_mesh_retire, suite : 'integration')
endif

# Scene GPU rendering with pixel assertions: offscreen surface
# (ADR-0013) + caller-owned depth. Covers depth testing and the
# Phong lit path end-to-end.
if opt_scene
  test_scene_render = executable('test_scene_render',
    'test_scene_render.c',
    dependencies : [test_helpers_dep],
  )
  test('scene_render', test_scene_render, suite : 'integration')
endif

# A generated, tiny VRM + VRMA pair keeps humanoid retargeting and animation
# sampling under CI without committing developer avatar assets.
if get_option('scene-graph')
  test_scene_graph_animation = executable('test_scene_graph_animation',
    'test_scene_graph_animation.c',
    dependencies : [test_helpers_dep, flux_sg_dep],
  )
  test('scene_graph_animation', test_scene_graph_animation, suite : 'integration')
endif

test_allocator = executable('test_allocator',
  'test_allocator.c',
  dependencies : [test_helpers_dep],
)
test('allocator', test_allocator, suite : 'integration')

# VK_EXT_memory_budget: enabled-when-advertised, per-heap usage/budget
# must be sane on supporting drivers and zeroed cleanly elsewhere.
test_memory_budget = executable('test_memory_budget',
  'test_memory_budget.c',
  dependencies : [test_helpers_dep],
)
test('memory_budget', test_memory_budget, suite : 'integration')

# OOM fault injection: interposed vkAllocateMemory drives the pool,
# dedicated, and reclaim-retry failure paths end to end.
test_alloc_fault = executable('test_alloc_fault',
  'test_alloc_fault.c',
  dependencies : [test_helpers_dep, dl_dep],
)
test('alloc_fault', test_alloc_fault, suite : 'integration')

# Batched uploads: one submission for a bulk load, auto-flush at frame
# begin, steady-state allocator accounting afterwards.
if opt_canvas
  test_upload_batch = executable('test_upload_batch',
    'test_upload_batch.c',
    dependencies : [test_helpers_dep],
  )
  test('upload_batch', test_upload_batch, suite : 'integration')
endif

# GPU integration: needs a compiled compute shader.
test_compute_golden = executable('test_compute_golden',
  'test_compute_golden.c',
  fill_pattern_spv,
  dependencies : [test_helpers_dep],
  c_args       : ['--embed-dir=' + core_tests_build_dir],
)
test('compute_golden', test_compute_golden, suite : 'integration')

test_validation = executable('test_validation',
  'test_validation.c',
  fill_pattern_spv,
  dependencies : [test_helpers_dep],
  c_args       : ['--embed-dir=' + core_tests_build_dir],
)
test('validation', test_validation, suite : 'integration', timeout : 30)

test_soak = executable('test_soak',
  'test_soak.c',
  triangle_vert_spv, triangle_frag_spv,
  dependencies : [test_helpers_dep],
  c_args       : ['--embed-dir=' + core_tests_build_dir],
)
test('soak', test_soak, suite : 'integration', timeout : 60)

# Offscreen surface + canvas pixel assertions (ADR-0013). Needs the
# canvas module for the drawing side of the round trip.
if opt_canvas
  test_offscreen = executable('test_offscreen',
    'test_offscreen.c',
    dependencies : [test_helpers_dep],
  )
  test('offscreen', test_offscreen, suite : 'integration')

  # Canvas feature matrix on the GPU: gradients, strokes, and the
  # hole-bridging fill (ADR-0011), asserted by read-back pixel.
  test_canvas_render = executable('test_canvas_render',
    'test_canvas_render.c',
    dependencies : [test_helpers_dep, flux_text_dep],
  )
  test('canvas_render', test_canvas_render, suite : 'integration')

  # Image retire queue: churn create -> draw -> release every frame so
  # inline destruction of in-flight images (GPU hang on i915) or a
  # missing bindless-slot recycle fails loudly.
  test_image_retire = executable('test_image_retire',
    'test_image_retire.c',
    dependencies : [test_helpers_dep],
  )
  test('image_retire', test_image_retire, suite : 'integration')
endif

# Built only when the effect module is enabled.
if opt_effect
  test_effect_blur = executable('test_effect_blur',
    'test_effect_blur.c',
    dependencies : [test_helpers_dep],
  )
  test('effect_blur', test_effect_blur, suite : 'integration')

  # Render-target capture (ADR-0017): capture -> blur -> composite.
  test_canvas_target = executable('test_canvas_target',
    'test_canvas_target.c',
    dependencies : [test_helpers_dep],
  )
  test('canvas_target', test_canvas_target, suite : 'integration')
endif
