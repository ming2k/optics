# libiris: the cross-platform L3 toolkit surface.
#
# Today's surface: iris_app_run drives a windowed Wayland event loop and
# dispatches per-frame build callbacks into lens. Theme query reads GNOME /
# freedesktop colour scheme at startup (and follows it live when libsystemd
# is available). File dialogs use xdg-desktop-portal over sd-bus. The a11y
# seam is a real AT-SPI bridge when libsystemd is available, a no-op stub
# otherwise.
#
# lens_dep (which transitively pulls flux_dep) is declared by libs/lens/
# meson.build when it is subdir()'d earlier from the root meson.build
# (ADR-0023 monorepo layout).
#
# Wayland-specific code lives in src/app_wayland.c; when a Win32 or Cocoa
# backend is added it will land as src/app_win32.c / src/app_cocoa.m and
# be selected at meson configure time.

iris_inc = include_directories('include')

# Cross-platform sources (compiled on every platform).
iris_sources = files(
  'src/version.c',
  'src/app.c',
  'src/theme_linux.c',
  'src/file_dialog_portal.c',
  'src/a11y_util.c',
)

iris_c_args = ['-DIRIS_BUILDING=1']
iris_extra_deps = []

# Live theme watching via sd-bus (org.freedesktop.portal.Settings SettingChanged).
# Optional: when absent, iris_watch_system_color_scheme returns -1 and callers
# fall back to the startup-only query.
sd_dep = dependency('libsystemd', required : false)
if sd_dep.found()
  iris_extra_deps += sd_dep
  iris_c_args += '-DIRIS_HAVE_PORTAL_WATCH=1'
  iris_c_args += '-DIRIS_HAVE_ATSPI=1'
  iris_sources += files('src/theme_watch_portal.c', 'src/a11y_atspi.c')
else
  iris_sources += files('src/theme_watch_stub.c', 'src/a11y_stub.c')
  message('libsystemd not found; live theme watching + AT-SPI bridge disabled')
endif

# Wayland backend (Linux only today).
is_linux = host_machine.cpu_family() != '' and (
  host_machine.system() == 'linux')
if is_linux
  wl_client_dep = dependency('wayland-client', required : false)
  xkb_dep       = dependency('xkbcommon',      required : false)
  vulkan_dep    = dependency('vulkan',         required : false)

  if wl_client_dep.found() and xkb_dep.found() and vulkan_dep.found()
    iris_sources += files('src/app_wayland.c')
    iris_extra_deps += [wl_client_dep, xkb_dep, vulkan_dep]
    iris_c_args += '-DIRIS_HAVE_WAYLAND=1'

    # Generate wayland protocol glue at build time. cursor-shape-v1
    # (wayland-protocols ≥ 1.32, Aug 2023) is the modern path: the
    # compositor renders the cursor with its own configured theme, so
    # we never load a cursor theme ourselves and the cursor always
    # matches the user's desktop. GNOME 45+, KDE Plasma 6, and wlroots
    # 0.18+ all support it; compositors that don't simply don't bind the
    # global, and iris_set_cursor falls back to no-op (compositor's
    # default arrow stays).
    #
    # We ship a local copy of cursor-shape-v1.xml with the
    # `get_tablet_tool_v2` request stripped: that request references
    # `zwp_tablet_tool_v2_interface`, which we don't bind, and wayland-
    # scanner generates an unresolved extern for it. Stripping the
    # request (we never call it; iris is mouse-only today) avoids the
    # link error without dragging in tablet-tool-v2.
    wl = import('wayland')
    iris_proto_src = wl.scan_xml(
      wl.find_protocol('xdg-shell'),
      wl.find_protocol('xdg-decoration', state : 'unstable', version : 1),
      wl.find_protocol('text-input', state : 'unstable', version : 3),
      wl.find_protocol('fractional-scale', state : 'staging', version : 1),
      'protocol/cursor-shape-v1.xml',
    )
    iris_sources += iris_proto_src
  else
    warning('Wayland backend skipped: wayland-client / xkbcommon / vulkan not found')
    iris_c_args += '-DIRIS_BUILD_NO_BACKEND=1'
  endif
else
  iris_c_args += '-DIRIS_BUILD_NO_BACKEND=1'
endif

iris_lib = library('iris',
  iris_sources,
  include_directories   : iris_inc,
  c_args                : iris_c_args,
  dependencies          : [lens_dep, m_dep] + iris_extra_deps,
  gnu_symbol_visibility : 'hidden',
  version               : meson.project_version(),
  install               : true,
)
install_headers(
  'include/iris/iris.h',
  'include/iris/app.h',
  'include/iris/cursor.h',
  'include/iris/theme.h',
  'include/iris/file_dialog.h',
  'include/iris/a11y.h',
  'include/iris/window.h',
  subdir : 'iris',
)

iris_dep = declare_dependency(
  link_with           : iris_lib,
  include_directories : iris_inc,
  dependencies        : [lens_dep],
)

pkg.generate(
  libraries   : iris_lib,
  name        : 'iris',
  filebase    : 'iris',
  description : 'L3 application toolkit for the flux/lens stack',
  version     : meson.project_version(),
  requires    : ['lens >= ' + meson.project_version()],
)
