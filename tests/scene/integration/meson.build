# scene GPU integration: the GLB parser is unit-tested without a device;
# binding an animation clip to a loaded scene needs a real gfx device.
test_scene_graph_animation = executable('test_scene_graph_animation',
  'test_scene_graph_animation.c',
  dependencies : [optics_gfx_test_support_dep, scene_dep],
)
test('scene_graph_animation', test_scene_graph_animation, suite : 'integration')

# End-to-end GPU rendering of parsed/built glTF scenes, material overrides,
# STEP/CUBICSPLINE animations, pose reset, and GPU skinning.
test_scene_render_pipeline = executable('test_scene_render_pipeline',
  'test_scene_render_pipeline.c',
  dependencies : [optics_gfx_test_support_dep, scene_dep],
)
test('scene_render_pipeline', test_scene_render_pipeline, suite : 'integration')
