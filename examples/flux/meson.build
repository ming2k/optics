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

# Windowed examples — all native Iris applications (ADR-0097).
executable('canvas_hello',
  'canvas_hello.c',
  dependencies : [flux_dep, iris_dep, m_dep],
)

executable('image_animation',
  'image_animation.c',
  dependencies : [flux_dep, iris_dep, m_dep],
)

executable('scene_cube',
  'scene_cube.c',
  dependencies : [flux_dep, iris_dep, m_dep],
)
