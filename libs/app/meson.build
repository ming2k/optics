# aperture — the application shell: lens running in an iris window.
#
# iris (window, events, frame cadence) depends on flux only; lens (headless UI
# engine) depends on no windowing system. This is the one library that links
# both, implementing iris's UI-host seam (<iris/host.h>) over a lens context.

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
  description : 'Application shell binding the lens UI engine to an iris window',
  version     : meson.project_version(),
  requires    : ['optics-window >= ' + meson.project_version(), 'optics-ui >= ' + meson.project_version()],
)
