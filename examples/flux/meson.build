glslang = find_program('glslangValidator', required : true)

# C23 #embed bakes SPIR-V into the example binaries; toolchains without it
# (MSVC cl, Apple Clang) take the tools/spv2h.py generated headers instead
# (same arrays, selected per TU via __has_embed). --embed-dir is probed
# because MSVC rejects unknown options outright.
examples_embed_dir_arg = cc.get_supported_arguments('--embed-dir=' + meson.current_build_dir())
spv2h_py = import('python').find_installation('python3')
spv2h    = files('../../tools/spv2h.py')

# Headless compute example — built unconditionally.
fill_comp_spv = custom_target('fill.comp.spv',
  input   : 'shaders/fill.comp',
  output  : 'fill.comp.spv',
  command : [glslang, '-V', '--target-env', 'vulkan1.3', '@INPUT@', '-o', '@OUTPUT@'],
)
fill_comp_h = custom_target('fill_comp_spv.h',
  input   : fill_comp_spv,
  output  : 'fill_comp_spv.h',
  command : [spv2h_py, spv2h, '@INPUT@', '@OUTPUT@', 'fill_spv'],
)
executable('compute_fill',
  'compute_fill.c',
  fill_comp_spv,
  fill_comp_h,
  dependencies : [flux_dep, m_dep],
  c_args       : examples_embed_dir_arg,
)

# Windowed examples — need GLFW. (Effect showcases live in examples/showcase/.)
if not glfw_dep.found()
  warning('GLFW not found; skipping windowed flux examples.')
  subdir_done()
endif

triangle_vert_spv = custom_target('triangle.vert.spv',
  input   : 'shaders/triangle.vert',
  output  : 'triangle.vert.spv',
  command : [glslang, '-V', '@INPUT@', '-o', '@OUTPUT@'],
)
triangle_frag_spv = custom_target('triangle.frag.spv',
  input   : 'shaders/triangle.frag',
  output  : 'triangle.frag.spv',
  command : [glslang, '-V', '@INPUT@', '-o', '@OUTPUT@'],
)
triangle_vert_h = custom_target('triangle_vert_spv.h',
  input   : triangle_vert_spv,
  output  : 'triangle_vert_spv.h',
  command : [spv2h_py, spv2h, '@INPUT@', '@OUTPUT@', 'triangle_vert_spv'],
)
triangle_frag_h = custom_target('triangle_frag_spv.h',
  input   : triangle_frag_spv,
  output  : 'triangle_frag_spv.h',
  command : [spv2h_py, spv2h, '@INPUT@', '@OUTPUT@', 'triangle_frag_spv'],
)
executable('hello_triangle',
  'hello_triangle.c',
  triangle_vert_spv,
  triangle_frag_spv,
  triangle_vert_h,
  triangle_frag_h,
  dependencies : [flux_dep, glfw_dep, m_dep],
  c_args       : examples_embed_dir_arg,
)

executable('canvas_hello',
  'canvas_hello.c',
  dependencies : [flux_dep, glfw_dep, m_dep],
)

executable('image_animation',
  'image_animation.c',
  dependencies : [flux_dep, glfw_dep, m_dep],
)

executable('scene_cube',
  'scene_cube.c',
  dependencies : [flux_dep, glfw_dep, m_dep],
)
