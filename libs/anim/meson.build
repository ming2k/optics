# anim — the shared motion vocabulary (ADR-0077). Pure math on
# caller-owned state: springs (closed-form, non-divergent by construction),
# approach/decay, easing, hysteresis, motion-adaptive smoothing. No clock,
# no timeline, no allocation. A leaf sibling: nothing else in the tree
# depends on it, and it depends on nothing but libm.

anim_inc = include_directories('include')

anim_sources = files(
  'src/anim.c',
)

anim_lib = library('anim',
  anim_sources,
  include_directories   : anim_inc,
  c_args                : ['-DANIM_BUILDING=1'],
  dependencies          : [m_dep],
  gnu_symbol_visibility : 'hidden',
  soversion             : optics_soname,
  version               : meson.project_version(),
  install               : true,
)

install_headers(
  'include/anim/anim.h',
  'include/anim/export.h',
  subdir : 'anim',
)

anim_dep = declare_dependency(
  link_with           : anim_lib,
  include_directories : anim_inc,
)

pkg.generate(
  libraries   : anim_lib,
  name        : 'anim',
  filebase    : 'anim',
  description : 'Shared motion vocabulary: springs, easing, hysteresis, adaptive smoothing (host-owned clocks)',
  version     : meson.project_version(),
)
