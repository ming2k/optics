# aperture — the application shell: lens running in an iris window.
#
# iris (window, events, frame cadence) depends on flux only; lens (headless UI
# engine) depends on no windowing system. This is the one library that links
# both, implementing iris's UI-host seam (<iris/host.h>) over a lens context.

aperture_inc = include_directories('include')

aperture_lib = library('aperture',
  'src/aperture.c',
  include_directories   : aperture_inc,
  c_args                : ['-DAPERTURE_BUILDING=1'],
  dependencies          : [iris_dep, lens_dep],
  gnu_symbol_visibility : 'hidden',
  soversion             : optics_soname,
  version               : meson.project_version(),
  install               : true,
)

install_headers('include/aperture/aperture.h', subdir : 'aperture')

aperture_dep = declare_dependency(
  link_with           : aperture_lib,
  include_directories : aperture_inc,
  dependencies        : [iris_dep, lens_dep],
)

pkg.generate(
  libraries   : aperture_lib,
  name        : 'aperture',
  filebase    : 'aperture',
  description : 'Application shell binding the lens UI engine to an iris window',
  version     : meson.project_version(),
  requires    : ['iris >= ' + meson.project_version(), 'lens >= ' + meson.project_version()],
)
