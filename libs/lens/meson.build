# liblens depends on libflux (rendering) and libflux-text (shaping, ADR-0016
# in flux) exclusively via pkg-config. This is the polyrepo contract: build
# and install flux and flux-text first, then build lens.


lens_inc = include_directories('include')

lens_sources = files(
  'src/core/context.c',
  'src/core/id.c',
  'src/store/store.c',
  'src/store/node.c',
  'src/layout/tree.c',
  'src/layout/solve.c',
  'src/input/input.c',
  'src/input/focus.c',
  'src/input/clipboard.c',
  'src/render/drawlist.c',
  'src/render/replay.c',
  'src/widgets/button.c',
  'src/widgets/selectable.c',
  'src/widgets/label.c',
  'src/widgets/title.c',
  'src/widgets/checkbox.c',
  'src/widgets/slider.c',
  'src/widgets/textfield.c',
  'src/widgets/collapsing.c',
  'src/widgets/dropdown.c',
  'src/widgets/radio.c',
  'src/widgets/scroll.c',
  'src/widgets/tabs.c',
  'src/widgets/textarea.c',
  'src/widgets/progress.c',
  'src/widgets/separator.c',
  'src/widgets/icon.c',
  'src/widgets/image.c',
  'src/widgets/modal.c',
  'src/widgets/menu.c',
  'src/widgets/split.c',
  'src/widgets/table.c',
  'src/icon_data.c',
  'src/a11y/semantics.c',
  'src/overlay/overlay.c',
  'src/theme/theme.c',
  'src/text/seam.c',
)

lens_lib = library('lens',
  lens_sources,
  include_directories   : lens_inc,
  c_args                : ['-DLENS_BUILDING=1'],
  dependencies          : [flux_dep, flux_text_dep, m_dep],
  gnu_symbol_visibility : 'hidden',
  version               : meson.project_version(),
  install               : true,
)

install_headers('include/lens/lens.h', 'include/lens/icon.h', subdir : 'lens')

lens_dep = declare_dependency(
  link_with           : lens_lib,
  include_directories : lens_inc,
  dependencies        : [flux_dep, flux_text_dep],
)

pkg.generate(
  libraries   : lens_lib,
  name        : 'lens',
  filebase    : 'lens',
  description : 'Immediate-mode UI over a retained core, built on flux',
  version     : meson.project_version(),
  requires    : ['flux >= 0.1.0', 'flux-text >= 0.1.0'],
)


