glslang = find_program('glslangValidator', required : true)

# Headless compute example — built unconditionally.
fill_comp_spv = custom_target('fill.comp.spv',
  input   : 'shaders/fill.comp',
  output  : 'fill.comp.spv',
  command : [glslang, '-V', '--target-env', 'vulkan1.3', '@INPUT@', '-o', '@OUTPUT@'],
)
executable('compute_fill',
  'compute_fill.c',
  fill_comp_spv,
  dependencies : [flux_dep, m_dep],
  c_args       : ['--embed-dir=' + meson.current_build_dir()],
)

# Windowed triangle example — needs GLFW.
if not glfw_dep.found()
  warning('GLFW not found; skipping hello_triangle example.')
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
executable('hello_triangle',
  'hello_triangle.c',
  triangle_vert_spv,
  triangle_frag_spv,
  dependencies : [flux_dep, glfw_dep, m_dep],
  c_args       : ['--embed-dir=' + meson.current_build_dir()],
)

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
executable('filament_plume',
  'filament_plume.c',
  filament_plume_vert_spv,
  filament_plume_frag_spv,
  dependencies : [flux_dep, glfw_dep, m_dep],
  c_args       : ['--embed-dir=' + meson.current_build_dir()],
)

executable('canvas_hello',
  'canvas_hello.c',
  dependencies : [flux_dep, glfw_dep, m_dep],
)

executable('image_animation',
  'image_animation.c',
  dependencies : [flux_dep, glfw_dep, m_dep],
)

# Liquid-glass backdrop-blur demo. Needs the effect module's Gaussian
# blur (flux_effect_blur), so it is gated on -Deffect=true.
if opt_effect
  executable('liquid_glass',
    'liquid_glass.c',
    dependencies : [flux_dep, glfw_dep, m_dep],
  )

  # Headless offscreen snapshot of the same effect (no window needed).
  executable('liquid_glass_shot',
    'liquid_glass_shot.c',
    dependencies : [flux_dep, m_dep],
  )
endif

executable('scene_cube',
  'scene_cube.c',
  dependencies : [flux_dep, glfw_dep, m_dep],
)

# Windowed particle-terrain example — needs GLFW + glslang.
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
executable('particles_terrain',
  'particles_terrain.c',
  terrain_vert_spv,
  terrain_frag_spv,
  dependencies : [flux_dep, glfw_dep, m_dep],
  c_args       : ['--embed-dir=' + meson.current_build_dir()],
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
executable('ripple_field',
  'ripple_field.c',
  ripple_field_vert_spv,
  ripple_field_frag_spv,
  dependencies : [flux_dep, glfw_dep, m_dep],
  c_args       : ['--embed-dir=' + meson.current_build_dir()],
)
