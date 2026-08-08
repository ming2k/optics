# liblens depends on libflux (rendering) and libflux-text (shaping) — both
# are siblings in this monorepo (ADR-0023). The flux_dep / flux_text_dep
# variables are declared by libs/flux/meson.build when it is subdir()'d
# earlier from the root meson.build.

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
  'src/widgets/switch.c',
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
  'src/widgets/tree.c',
  'src/icon_data.c',
  'src/icon_runtime.c',
  'src/vendor/nanosvg.c',
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
  requires    : ['flux >= ' + meson.project_version(), 'flux-text >= ' + meson.project_version()],
)

