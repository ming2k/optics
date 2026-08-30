# libiris: the cross-platform L3 toolkit surface.
#
# iris_app_run drives a windowed event loop and dispatches per-frame build
# callbacks into lens. Theme query reads the OS colour-scheme preference at
# startup and follows it live where supported. File dialogs use
# xdg-desktop-portal on Linux. The a11y seam is a real AT-SPI bridge on
# Linux when libsystemd is available, a no-op stub otherwise.
#
# Backend selection is compile-time (ADR-0044): exactly one of the
# per-OS branches below adds its backend sources and defines
# IRIS_BACKEND_WAYLAND / IRIS_BACKEND_WIN32 / IRIS_BACKEND_COCOA, which the
# dispatcher in src/app.c switches on. Platforms without a backend get
# IRIS_BUILD_NO_BACKEND: a linkable libiris whose iris_app_run returns
# non-zero, for platform-less CI / bindgen builds. The public headers are
# identical on every platform.
#
# lens_dep (which transitively pulls flux_dep) is declared by libs/lens/
# meson.build when it is subdir()'d earlier from the root meson.build
# (ADR-0023 monorepo layout).

iris_inc = include_directories('include')

# Cross-platform sources (compiled on every platform).
iris_sources = files(
  'src/version.c',
  'src/app.c',
  'src/capability.c',
  'src/a11y_util.c',
  'src/platform_text.c',
  'src/uri.c',
)

iris_c_args = ['-DIRIS_BUILDING=1']
iris_objc_args = []
iris_extra_deps = []

host_system = host_machine.system()

if host_system == 'linux'
  # ---- Linux: Wayland backend + portal/sd-bus system integration --------
  #
  # Theme query (gsettings/GTK_THEME), the portal file dialog, the
  # cross-thread wakeup queue, and (with libsystemd) the live theme watcher
  # + AT-SPI bridge are the Linux implementations of the platform-neutral
  # public APIs. The watcher and bridge pump sd-bus: the watcher on its
  # own thread (delivering via the wakeup seam), the bridge through the
  # backend's poll set (src/a11y_internal.h integration point).
  iris_sources += files(
    'src/theme_linux.c',
    'src/file_dialog_portal.c',
    'src/platform_wakeup.c',
  )
  iris_extra_deps += dependency('threads')

  # Live theme watching + AT-SPI bridge via sd-bus. Optional: when absent,
  # stubs report the features as unavailable and callers fall back to the
  # startup-only query / no a11y transport.
  sd_dep = dependency('libsystemd', required : false)
  if sd_dep.found()
    iris_extra_deps += sd_dep
    iris_c_args += ['-DIRIS_HAVE_PORTAL_WATCH=1', '-DIRIS_HAVE_ATSPI=1']
    iris_sources += files('src/theme_watch_portal.c', 'src/a11y_atspi.c')
  else
    iris_sources += files('src/theme_watch_stub.c', 'src/a11y_stub.c', 'src/a11y_prefs_stub.c')
    message('libsystemd not found; live theme watching + AT-SPI bridge disabled')
  endif
  # The a11y preference query shells gsettings/kreadconfig (no D-Bus client
  # dependency) and compiles on every Linux build, portal or not.
  iris_sources += files('src/a11y_prefs_linux.c')

  wl_client_dep = dependency('wayland-client', required : false)
  xkb_dep       = dependency('xkbcommon',      required : false)
  vulkan_dep    = dependency('vulkan',         required : false)

  if wl_client_dep.found() and xkb_dep.found() and vulkan_dep.found()
    iris_sources += files('src/app_wayland.c')
    iris_extra_deps += [wl_client_dep, xkb_dep, vulkan_dep]
    iris_c_args += '-DIRIS_BACKEND_WAYLAND=1'

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
      wl.find_protocol('xdg-foreign', state : 'unstable', version : 2),
      wl.find_protocol('text-input', state : 'unstable', version : 3),
      wl.find_protocol('fractional-scale', state : 'staging', version : 1),
      wl.find_protocol('primary-selection', state : 'unstable', version : 1),
      # Pen/tablet input (tablet_wayland.c): pressure, eraser end, barrel
      # buttons. Compositors without tablets simply don't advertise the
      # global; the bridge stays inert.
      wl.find_protocol('tablet', state : 'unstable', version : 2),
      'protocol/cursor-shape-v1.xml',
    )
    iris_sources += iris_proto_src
    iris_sources += files('src/tablet_wayland.c')
  else
    warning('Wayland backend skipped: wayland-client / xkbcommon / vulkan not found')
    iris_c_args += '-DIRIS_BUILD_NO_BACKEND=1'
  endif

elif host_system == 'windows'
  # ---- Windows: Win32 backend -------------------------------------------
  # The contract these files implement is documented in
  # src/platform_internal.h (+ the Win32 recipe in
  # src/platform_wakeup.h). theme_win32.c supplies the theme.h API
  # (query + callback-based watch); file_dialog_win32.c the file_dialog.h
  # API; a11y_stub.c keeps the bridge inert until a UI Automation bridge
  # lands. platform_wakeup.c uses a portable lock (SRWLOCK on _WIN32).
  iris_sources += files(
    'src/app_win32.c',
    'src/theme_win32.c',
    'src/file_dialog_win32.c',
    'src/a11y_stub.c',
    'src/a11y_prefs_win32.c',
    'src/platform_wakeup.c',
  )
  iris_c_args += '-DIRIS_BACKEND_WIN32=1'
  # System libraries: windowing (user32/gdi32), IME (imm32), COM file
  # dialogs (ole32/shell32/uuid), registry theme query (advapi32).
  cc = meson.get_compiler('c')
  foreach winlib : ['user32', 'gdi32', 'imm32', 'ole32', 'shell32', 'uuid', 'advapi32']
    iris_extra_deps += cc.find_library(winlib, required : true)
  endforeach

elif host_system == 'darwin'
  # ---- macOS: Cocoa backend ----------------------------------------------
  # Same situation as the Win32 branch: files land with the Cocoa backend
  # task. platform_wakeup.c is usable as-is (macOS has native pthreads).
  add_languages('objc', required : true)
  iris_sources += files(
    'src/app_cocoa.m',
    'src/theme_cocoa.m',
    'src/file_dialog_cocoa.m',
    'src/a11y_stub.c',
    'src/a11y_prefs_cocoa.m',
    'src/platform_wakeup.c',
  )
  iris_c_args += '-DIRIS_BACKEND_COCOA=1'
  iris_objc_args += ['-fobjc-arc']
  # AppKit (window/events/IME), QuartzCore (CAMetalLayer for the Vulkan
  # surface), Foundation comes in via Cocoa.
  iris_extra_deps += dependency('appleframeworks',
                                modules : ['Cocoa', 'QuartzCore', 'CoreFoundation'])

else
  # ---- Anything else: linkable shell, no backend -------------------------
  # theme_linux.c and file_dialog_portal.c are internally platform-gated
  # (safe defaults / portal-disabled stubs) and compile anywhere; the watch
  # and a11y seams get their stubs.
  iris_c_args += '-DIRIS_BUILD_NO_BACKEND=1'
  iris_sources += files(
    'src/theme_linux.c',
    'src/file_dialog_portal.c',
    'src/theme_watch_stub.c',
    'src/a11y_stub.c',
    'src/a11y_prefs_linux.c',
    'src/a11y_prefs_stub.c',
  )
endif

iris_lib = library('iris',
  iris_sources,
  include_directories   : iris_inc,
  c_args                : iris_c_args,
  objc_args             : iris_objc_args,
  dependencies          : [lens_dep, m_dep] + iris_extra_deps,
  gnu_symbol_visibility : 'hidden',
  soversion             : optics_soname,
  version               : meson.project_version(),
  install               : true,
)
install_headers(
  'include/iris/iris.h',
  'include/iris/app.h',
  'include/iris/capability.h',
  'include/iris/cursor.h',
  'include/iris/dnd.h',
  'include/iris/theme.h',
  'include/iris/file_dialog.h',
  'include/iris/a11y.h',
  'include/iris/a11y_prefs.h',
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
