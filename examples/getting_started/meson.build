# examples/getting_started — minimal, single-concept starters (ADR-0171).

if opt_ui
  executable('hello_app',
    'hello_app.c',
    dependencies : [app_dep, window_dep, ui_dep],
    install      : false,
  )
endif

executable('canvas_window',
  'canvas_window.c',
  dependencies : [window_dep, gfx_dep, m_dep],
  install      : false,
)

executable('headless_canvas',
  'headless_canvas.c',
  dependencies : [gfx_dep],
  install      : false,
)

if opt_svg
  executable('headless_svg',
    'headless_svg.c',
    dependencies : [svg_dep],
    install      : false,
  )
endif

# Compute shader starter (Slang -> SPIR-V)
gs_slangc = find_program('slangc', required : true)
gs_slang_flags = ['-target', 'spirv', '-profile', 'spirv_1_6', '-Wno-41012']
gs_embed_args = ['--embed-dir=' + meson.current_build_dir()]

fill_spv = custom_target('gs_fill.comp.spv',
  input   : 'shaders/fill.slang',
  output  : 'fill.comp.spv',
  command : [gs_slangc, gs_slang_flags, '-entry', 'main', '-stage', 'compute', '@INPUT@', '-o', '@OUTPUT@'],
)

executable('compute_pass',
  ['compute_pass.c', fill_spv],
  c_args       : gs_embed_args,
  dependencies : [gfx_dep, vulkan_dep],
  install      : false,
)
