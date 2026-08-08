# Showcase examples — composition effect demos.
#
# Here the effect itself (visual or mathematical) is the point, not API
# teaching. Functional API examples live per library in examples/<library>/.

glslang = find_program('glslangValidator', required : true)

# Headless liquid-glass A/B harness — offscreen surface + PPM readback,
# no GLFW. Gated on -Deffect=true like the windowed demo.
if opt_effect
  executable('liquid_glass_study',
    'liquid_glass_study.c',
    dependencies : [flux_dep, m_dep],
  )
endif

# All remaining showcases are windowed (GLFW).
if not glfw_dep.found()
  warning('GLFW not found; skipping windowed showcase examples.')
  subdir_done()
endif

# The windowed showcases share pipeline_cache.h with the flux examples; the
# single canonical copy lives in examples/flux/ (it is written to be copied
# or included by downstream code).
flux_examples_inc = include_directories('../flux')

# C23 #embed bakes SPIR-V into the binaries; toolchains without it (MSVC
# cl, Apple Clang) take the tools/spv2h.py generated headers instead (same
# arrays, selected per TU via __has_embed). --embed-dir is probed because
# MSVC rejects unknown options outright.
showcase_embed_dir_arg = cc.get_supported_arguments('--embed-dir=' + meson.current_build_dir())
spv2h_py = import('python').find_installation('python3')
spv2h    = files('../../tools/spv2h.py')

filament_plume_vert_spv = custom_target('filament_plume.vert.spv',
  input   : 'shaders/filament_plume.vert',
  output  : 'filament_plume.vert.spv',
  command : [glslang, '-V', '@INPUT@', '-o', '@OUTPUT@'],
)
filament_plume_frag_spv = custom_target('filament_plume.frag.spv',
  input   : 'shaders/filament_plume.frag',
  output  : 'filament_plume.frag.spv',
  command : [glslang, '-V', '@INPUT@', '-o', '@OUTPUT@'],
)
filament_plume_vert_h = custom_target('filament_plume_vert_spv.h',
  input   : filament_plume_vert_spv,
  output  : 'filament_plume_vert_spv.h',
  command : [spv2h_py, spv2h, '@INPUT@', '@OUTPUT@', 'filament_plume_vert_spv'],
)
filament_plume_frag_h = custom_target('filament_plume_frag_spv.h',
  input   : filament_plume_frag_spv,
  output  : 'filament_plume_frag_spv.h',
  command : [spv2h_py, spv2h, '@INPUT@', '@OUTPUT@', 'filament_plume_frag_spv'],
)
executable('filament_plume',
  'filament_plume.c',
  filament_plume_vert_spv,
  filament_plume_frag_spv,
  filament_plume_vert_h,
  filament_plume_frag_h,
  dependencies        : [flux_dep, glfw_dep, m_dep],
  include_directories : flux_examples_inc,
  c_args              : showcase_embed_dir_arg,
)

# Liquid-glass backdrop-blur demo. Needs the effect module's Gaussian
# blur (flux_effect_blur), so it is gated on -Deffect=true.
if opt_effect
  executable('liquid_glass',
    'liquid_glass.c',
    dependencies        : [flux_dep, glfw_dep, m_dep],
    include_directories : flux_examples_inc,
  )
endif

terrain_vert_spv = custom_target('particles_terrain.vert.spv',
  input   : 'shaders/particles_terrain.vert',
  output  : 'particles_terrain.vert.spv',
  command : [glslang, '-V', '@INPUT@', '-o', '@OUTPUT@'],
)
terrain_frag_spv = custom_target('particles_terrain.frag.spv',
  input   : 'shaders/particles_terrain.frag',
  output  : 'particles_terrain.frag.spv',
  command : [glslang, '-V', '@INPUT@', '-o', '@OUTPUT@'],
)
terrain_vert_h = custom_target('particles_terrain_vert_spv.h',
  input   : terrain_vert_spv,
  output  : 'particles_terrain_vert_spv.h',
  command : [spv2h_py, spv2h, '@INPUT@', '@OUTPUT@', 'terrain_vert_spv'],
)
terrain_frag_h = custom_target('particles_terrain_frag_spv.h',
  input   : terrain_frag_spv,
  output  : 'particles_terrain_frag_spv.h',
  command : [spv2h_py, spv2h, '@INPUT@', '@OUTPUT@', 'terrain_frag_spv'],
)
executable('particles_terrain',
  'particles_terrain.c',
  terrain_vert_spv,
  terrain_frag_spv,
  terrain_vert_h,
  terrain_frag_h,
  dependencies        : [flux_dep, glfw_dep, m_dep],
  include_directories : flux_examples_inc,
  c_args              : showcase_embed_dir_arg,
)

ripple_field_vert_spv = custom_target('ripple_field.vert.spv',
  input   : 'shaders/ripple_field.vert',
  output  : 'ripple_field.vert.spv',
  command : [glslang, '-V', '@INPUT@', '-o', '@OUTPUT@'],
)
ripple_field_frag_spv = custom_target('ripple_field.frag.spv',
  input   : 'shaders/ripple_field.frag',
  output  : 'ripple_field.frag.spv',
  command : [glslang, '-V', '@INPUT@', '-o', '@OUTPUT@'],
)
ripple_field_vert_h = custom_target('ripple_field_vert_spv.h',
  input   : ripple_field_vert_spv,
  output  : 'ripple_field_vert_spv.h',
  command : [spv2h_py, spv2h, '@INPUT@', '@OUTPUT@', 'fx_vert_spv'],
)
ripple_field_frag_h = custom_target('ripple_field_frag_spv.h',
  input   : ripple_field_frag_spv,
  output  : 'ripple_field_frag_spv.h',
  command : [spv2h_py, spv2h, '@INPUT@', '@OUTPUT@', 'fx_frag_spv'],
)
executable('ripple_field',
  'ripple_field.c',
  ripple_field_vert_spv,
  ripple_field_frag_spv,
  ripple_field_vert_h,
  ripple_field_frag_h,
  dependencies        : [flux_dep, glfw_dep, m_dep],
  include_directories : flux_examples_inc,
  c_args              : showcase_embed_dir_arg,
)

# Mathematical showcase — compute pipeline writes a storage image each frame
# and the canvas presents it, so it needs both modules.
if opt_compute and opt_canvas
  julia_morph_comp_spv = custom_target('julia_morph.comp.spv',
    input   : 'shaders/julia_morph.comp',
    output  : 'julia_morph.comp.spv',
    command : [glslang, '-V', '--target-env', 'vulkan1.3', '@INPUT@', '-o', '@OUTPUT@'],
  )
  julia_morph_comp_h = custom_target('julia_morph_comp_spv.h',
    input   : julia_morph_comp_spv,
    output  : 'julia_morph_comp_spv.h',
    command : [spv2h_py, spv2h, '@INPUT@', '@OUTPUT@', 'julia_morph_comp_spv'],
  )
  executable('julia_morph',
    'julia_morph.c',
    julia_morph_comp_spv,
    julia_morph_comp_h,
    dependencies        : [flux_dep, glfw_dep, m_dep],
    include_directories : flux_examples_inc,
    c_args              : showcase_embed_dir_arg,
  )
endif
