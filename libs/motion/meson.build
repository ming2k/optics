# transit — the shared motion and state transition vocabulary (ADR-0077, ADR-0106).
# Pure math on caller-owned state: springs (closed-form, non-divergent by construction),
# approach/decay, easing, hysteresis, motion-adaptive smoothing. No clock,
# no timeline, no allocation. Header-only library; depends on nothing but libm.

motion_inc = include_directories('include')

install_headers('include/optics/motion.h', subdir : 'optics')
install_headers(
    'include/optics/motion/export.h',
  subdir : 'optics/motion',
)

motion_dep = declare_dependency(
  include_directories : motion_inc,
  dependencies        : [m_dep],
)

pkg.generate(
  libraries   : motion_dep,
  name        : 'optics-motion',
  filebase    : 'optics-motion',
  description : 'Shared motion and transition vocabulary: springs, easing, hysteresis, adaptive smoothing (header-only)',
  version     : meson.project_version(),
)
