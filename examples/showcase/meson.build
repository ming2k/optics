# Showcase examples — flagship material & composition effect demos (ADR-0097).
#
# Unlike per-library starter examples, showcases demonstrate cross-module
# full-stack integration (Flux + Prism + Lens + Iris).

if opt_effect and get_option('prism')
  executable('material_gallery',
    'material_gallery.c',
    dependencies : [flux_dep, prism_dep, lens_dep, iris_dep, m_dep],
  )
endif
