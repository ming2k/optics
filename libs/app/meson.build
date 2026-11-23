# app — the application shell: ui running in a native window.
#
# window (window, events, frame cadence) depends on gfx only; ui (headless UI
# engine) depends on no windowing system. This is the one library that links
# both, implementing window's UI-host seam (<optics/window/host.h>) over a ui context.

app_inc = include_directories('include')

app_lib = library('optics_app',
  'src/app.c',
  include_directories   : app_inc,
  c_args                : ['-DOPTICS_APP_BUILDING=1'],
  dependencies          : [window_dep, ui_dep],
  gnu_symbol_visibility : 'hidden',
  soversion             : optics_soname,
  version               : meson.project_version(),
  install               : true,
)

install_headers('include/optics/app.h', subdir : 'optics')

app_dep = declare_dependency(
  link_with           : app_lib,
  include_directories : app_inc,
  dependencies        : [window_dep, ui_dep],
)

pkg.generate(
  libraries   : app_lib,
  name        : 'optics-app',
  filebase    : 'optics-app',
  description : 'Application shell binding the UI engine to a native window',
  version     : meson.project_version(),
  requires    : ['optics-window >= ' + meson.project_version(), 'optics-ui >= ' + meson.project_version()],
)
