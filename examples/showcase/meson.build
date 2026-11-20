# examples/showcase — flagship Optics desktop applications (ADR-0171).

if opt_ui and opt_motion
  executable('widget_gallery',
    'widget_gallery/main.c',
    'widget_gallery/page_typography.c',
    'widget_gallery/page_controls.c',
    'widget_gallery/page_layout_data.c',
    'widget_gallery/page_overlays_dnd.c',
    'widget_gallery/page_motion.c',
    dependencies : [app_dep, window_dep, ui_dep, motion_dep, m_dep],
    install      : false,
  )
endif

if opt_ui and opt_material
  executable('material_gallery',
    'material_gallery/main.c',
    'material_gallery/ui.c',
    'material_gallery/backdrops.c',
    'material_gallery/view_liquid_glass.c',
    'material_gallery/view_frosted.c',
    'material_gallery/view_acrylic.c',
    'material_gallery/view_mica.c',
    dependencies : [gfx_dep, material_dep, ui_dep, app_dep, m_dep],
    install      : false,
  )
endif

if opt_ui and opt_scene
  studio_duck = meson.current_source_dir() / 'scene_studio/assets/Duck.glb'
  executable('scene_studio',
    'scene_studio/main.c',
    c_args       : ['-DOPTICS_STUDIO_DUCK_ASSET="' + studio_duck + '"'],
    dependencies : [app_dep, window_dep, ui_dep, scene_dep, gfx_dep, vulkan_dep, m_dep],
    install      : false,
  )
endif
