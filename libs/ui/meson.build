# liboptics_ui depends on liboptics_gfx (rendering) and liboptics_text (shaping) — both
# are siblings in this monorepo (ADR-0023). The gfx_dep / text_dep
# variables are declared by libs/gfx/meson.build when it is subdir()'d
# earlier from the root meson.build.

ui_inc = include_directories('include')

ui_sources = files(
  'src/core/context.c',
  'src/core/id.c',
  'src/core/borrow.c',
  'src/store/store.c',
  'src/store/node.c',
  'src/layout/tree.c',
  'src/layout/solve.c',
  'src/layout/flex.c',
  'src/layout/grid.c',
  'src/input/input.c',
  'src/input/focus.c',
  'src/input/clipboard.c',
  'src/input/dnd.c',
  'src/render/drawlist.c',
  'src/render/replay.c',
  'src/render/ghost.c',
  'src/render/icon_mask.c',
  'src/style/style.c',
  'src/skin/skin.c',
  'src/skin/button.c',
  'src/skin/selectable.c',
  'src/skin/checkbox.c',
  'src/skin/slider.c',
  'src/skin/label.c',
  'src/skin/separator.c',
  'src/skin/icon.c',
  'src/skin/image.c',
  'src/skin/textedit.c',
  'src/skin/scrollbar.c',
  'src/widgets/button.c',
  'src/widgets/selectable.c',
  'src/widgets/label.c',
  'src/widgets/checkbox.c',
  'src/widgets/slider.c',
  'src/widgets/textedit.c',
  'src/widgets/scroll.c',
  'src/widgets/separator.c',
  'src/widgets/icon.c',
  'src/widgets/image.c',
  'src/patterns/segmented.c',
  'src/patterns/virtual.c',
  'src/patterns/tab_strip.c',
  'src/patterns/split_pane.c',
  'src/patterns/datagrid.c',
  'src/patterns/tree_view.c',
  'src/patterns/menu.c',
  'src/icon_runtime.c',
  'src/a11y/semantics.c',
  'src/place/place.c',
  'src/theme/theme.c',
  'src/text/seam.c',
)

ui_lib = library('optics_ui',
  ui_sources,
  include_directories   : ui_inc,
  # OPTICS_UI_DEBUG_BORROWS (ADR-0084): the borrowed-string registry is always
  # on in the shipped library — its cost is one bounded array walk per
  # label (nanoseconds) and its abort-on-violation catches the single
  # most common ui misuse (stack temporaries) in debug AND release
  # alike. Builds that must not abort can define OPTICS_UI_NO_BORROW_CHECK.
  c_args                : ['-DOPTICS_UI_BUILDING=1', '-DOPTICS_UI_DEBUG_BORROWS=1'],
  dependencies          : [gfx_dep, text_dep, m_dep],
  gnu_symbol_visibility : 'hidden',
  soversion             : optics_soname,
  version               : meson.project_version(),
  install               : true,
)

install_headers('include/optics/ui.h', subdir : 'optics')
install_headers(
    'include/optics/ui/patterns.h',
  'include/optics/ui/icon.h',
  'include/optics/ui/export.h',
  subdir : 'optics/ui',
)

ui_dep = declare_dependency(
  link_with           : ui_lib,
  include_directories : ui_inc,
  dependencies        : [gfx_dep, text_dep],
)

pkg.generate(
  libraries   : ui_lib,
  name        : 'optics-ui',
  filebase    : 'optics-ui',
  description : 'Immediate-mode UI over a retained core, built on gfx',
  version     : meson.project_version(),
  requires    : ['optics-gfx >= ' + meson.project_version(), 'optics-text >= ' + meson.project_version()],
)

