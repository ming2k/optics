# transit — the shared motion and state transition vocabulary (ADR-0077, ADR-0106).
# Pure math on caller-owned state: springs (closed-form, non-divergent by construction),
# approach/decay, easing, hysteresis, motion-adaptive smoothing. No clock,
# no timeline, no allocation. A leaf sibling: nothing else in the tree
# depends on it, and it depends on nothing but libm.

transit_inc = include_directories('include')

transit_sources = files(
  'src/transit.c',
)

transit_lib = library('transit',
  transit_sources,
  include_directories   : transit_inc,
  c_args                : ['-DTRANSIT_BUILDING=1'],
  dependencies          : [m_dep],
  gnu_symbol_visibility : 'hidden',
  soversion             : optics_soname,
  version               : meson.project_version(),
  install               : true,
)

install_headers(
  'include/transit/transit.h',
  'include/transit/export.h',
  subdir : 'transit',
)

transit_dep = declare_dependency(
  link_with           : transit_lib,
  include_directories : transit_inc,
)

pkg.generate(
  libraries   : transit_lib,
  name        : 'transit',
  filebase    : 'transit',
  description : 'Shared motion and transition vocabulary: springs, easing, hysteresis, adaptive smoothing',
  version     : meson.project_version(),
)
