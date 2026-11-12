# transit — the shared motion and state transition vocabulary (ADR-0077, ADR-0106).
# Pure math on caller-owned state: springs (closed-form, non-divergent by construction),
# approach/decay, easing, hysteresis, motion-adaptive smoothing. No clock,
# no timeline, no allocation. Header-only library; depends on nothing but libm.

transit_inc = include_directories('include')

install_headers(
  'include/transit/transit.h',
  'include/transit/export.h',
  subdir : 'transit',
)

transit_dep = declare_dependency(
  include_directories : transit_inc,
  dependencies        : [m_dep],
)

pkg.generate(
  libraries   : transit_dep,
  name        : 'transit',
  filebase    : 'transit',
  description : 'Shared motion and transition vocabulary: springs, easing, hysteresis, adaptive smoothing (header-only)',
  version     : meson.project_version(),
)
