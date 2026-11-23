# scene GPU integration: the GLB parser is unit-tested without a device;
# binding an animation clip to a loaded scene needs a real gfx device.
test_scene_graph_animation = executable('test_scene_graph_animation',
  'test_scene_graph_animation.c',
  dependencies : [optics_gfx_test_support_dep, scene_dep],
)
test('scene_graph_animation', test_scene_graph_animation, suite : 'integration')
