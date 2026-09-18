# Showcase examples — flagship material & composition effect demos (ADR-0097).
#
# Unlike per-library starter examples, showcases demonstrate cross-module
# full-stack integration (Flux + Prism + Lens + Iris).

if opt_effect and get_option('prism')
  executable('material_gallery',
    'material_gallery/main.c',
    'material_gallery/ui.c',
    'material_gallery/backdrops.c',
    'material_gallery/view_grid.c',
    'material_gallery/view_liquid_glass.c',
    'material_gallery/view_frosted.c',
    'material_gallery/view_acrylic.c',
    'material_gallery/view_mica.c',
    dependencies : [flux_dep, prism_dep, lens_dep, iris_dep, m_dep],
  )
endif
