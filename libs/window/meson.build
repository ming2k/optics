# liboptics_window: the platform shell — native window, event pump, frame
# cadence, clipboard, IME, dialogs and the accessibility bridge.
#
# window depends on gfx and nothing above it. optics_window_run drives a
# windowed event loop and presents host canvas content plus an optional UI
# layer that plugs in through <optics/window/host.h>; `app` binds ui to that
# seam (ADR-0166). The a11y bridge is AT-SPI on Linux when libsystemd is
# available and a no-op stub elsewhere.
#
# Backend selection is compile-time (ADR-0044): exactly one of the per-OS
# branches below adds its backend sources and defines
# OPTICS_WINDOW_BACKEND_{WAYLAND,WIN32,COCOA}, which src/app.c dispatches on.
# Platforms without a backend get OPTICS_WINDOW_BUILD_NO_BACKEND: a linkable
# library whose run entry point fails, for platform-less CI and bindgen
# builds. The public headers are identical on every platform.

window_inc = include_directories('include')

# Cross-platform sources (compiled on every platform).
window_sources = files(
  'src/version.c',
  'src/app.c',
  'src/event.c',
  'src/capability.c',
  'src/a11y_util.c',
  'src/platform_text.c',
  'src/uri.c',
  # The frame-cadence state machine (ADR-0122). Platform-neutral and pure:
  # each backend feeds it its own VSync source (wl_surface.frame, the macOS
  # display link, the Win32 waitable swapchain) and it returns the poll
  # budget plus the paint/skip/quiesce decision. Keeping it out of the
  # backend files is what makes the no-spin guarantee testable headlessly.
  'src/display_pacer.c',
  # The lockless triple-buffered snapshot exchange mailbox (ADR-0124).
  # Decouples the UI thread from the compositor presentation thread.
  'src/frame_mailbox.c',
)

window_c_args = ['-DOPTICS_WINDOW_BUILDING=1']
window_objc_args = []
window_extra_deps = []

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
  window_sources += files(
    'src/theme_linux.c',
    'src/file_dialog_portal.c',
    'src/platform_wakeup.c',
  )
  window_extra_deps += dependency('threads')

  # Live theme watching + AT-SPI bridge via sd-bus. Optional: when absent,
  # stubs report the features as unavailable and callers fall back to the
  # startup-only query / no a11y transport.
  sd_dep = dependency('libsystemd', required : false)
  if sd_dep.found()
    window_extra_deps += sd_dep
    window_c_args += ['-DOPTICS_WINDOW_HAVE_PORTAL_WATCH=1', '-DOPTICS_WINDOW_HAVE_ATSPI=1']
    window_sources += files('src/theme_watch_portal.c', 'src/a11y_atspi.c')
  else
    window_sources += files('src/theme_watch_stub.c', 'src/a11y_stub.c', 'src/a11y_prefs_stub.c')
    message('libsystemd not found; live theme watching + AT-SPI bridge disabled')
  endif
  # The a11y preference query shells gsettings/kreadconfig (no D-Bus client
  # dependency) and compiles on every Linux build, portal or not.
  window_sources += files('src/a11y_prefs_linux.c')

  wl_client_dep = dependency('wayland-client', required : false)
  xkb_dep       = dependency('xkbcommon',      required : false)
  vulkan_dep    = dependency('vulkan',         required : false)

  if wl_client_dep.found() and xkb_dep.found() and vulkan_dep.found()
    window_sources += files('src/app_wayland.c', 'src/wayland_data.c')
    window_extra_deps += [wl_client_dep, xkb_dep, vulkan_dep]
    window_c_args += '-DOPTICS_WINDOW_BACKEND_WAYLAND=1'

    # Generate wayland protocol glue at build time. cursor-shape-v1
    # (wayland-protocols >= 1.32) lets the compositor draw the cursor from the
    # user's own theme, for the pointer and for tablet tools; compositors
    # without it don't advertise the global and the cursor calls are no-ops.
    # Its get_tablet_tool_v2 request references the tablet protocol scanned
    # below, so the two are generated together.
    #
    # text-input-unstable-v3.xml is vendored: the listener fills every
    # v2/v3 event slot (libwayland aborts on a NULL slot), while older host
    # protocol packages — including Ubuntu 24.04's — predate `preedit_hint`,
    # `action`, and `language`. Pinning the XML keeps the generated listener
    # and app_wayland.c's slot contract in lockstep.
    wl = import('wayland')
    optics_window_proto_src = wl.scan_xml(
      wl.find_protocol('xdg-shell'),
      wl.find_protocol('xdg-decoration', state : 'unstable', version : 1),
      wl.find_protocol('xdg-foreign', state : 'unstable', version : 2),
      'protocol/text-input-unstable-v3.xml',
      wl.find_protocol('fractional-scale', state : 'staging', version : 1),
      wl.find_protocol('primary-selection', state : 'unstable', version : 1),
      wl.find_protocol('xdg-activation', state : 'staging', version : 1),
      # presentation-time (stable): the compositor's own report of when a
      # frame actually reached the display, plus the real refresh interval.
      # This is one of the two clocks ADR-0122 paces from (the other being
      # wl_surface.frame); it is what lets window learn 120/144 Hz instead of
      # assuming 60 Hz.
      wl.find_protocol('presentation-time'),
      # Pen/tablet input (tablet_wayland.c): pressure, eraser end, barrel
      # buttons. Compositors without tablets simply don't advertise the
      # global; the bridge stays inert.
      wl.find_protocol('tablet', state : 'unstable', version : 2),
      wl.find_protocol('cursor-shape', state : 'staging', version : 1),
    )
    window_sources += optics_window_proto_src
    window_sources += files('src/tablet_wayland.c')
  else
    warning('Wayland backend skipped: wayland-client / xkbcommon / vulkan not found')
    window_c_args += '-DOPTICS_WINDOW_BUILD_NO_BACKEND=1'
  endif

elif host_system == 'windows'
  # ---- Windows: Win32 backend -------------------------------------------
  # The contract these files implement is src/platform_internal.h (plus the
  # Win32 notes in src/platform_wakeup.h). a11y uses the stub until a UI
  # Automation bridge exists.
  window_sources += files(
    'src/app_win32.c',
    'src/theme_win32.c',
    'src/file_dialog_win32.c',
    'src/a11y_stub.c',
    'src/a11y_prefs_win32.c',
    'src/platform_wakeup.c',
  )
  window_c_args += '-DOPTICS_WINDOW_BACKEND_WIN32=1'
  # System libraries: windowing (user32/gdi32), IME (imm32), COM file
  # dialogs (ole32/shell32/uuid), registry theme query (advapi32).
  cc = meson.get_compiler('c')
  foreach winlib : ['user32', 'gdi32', 'imm32', 'ole32', 'shell32', 'uuid', 'advapi32', 'dwmapi']
    window_extra_deps += cc.find_library(winlib, required : true)
  endforeach

elif host_system == 'darwin'
  # ---- macOS: Cocoa backend ----------------------------------------------
  # The Vulkan surface is a CAMetalLayer driven through MoltenVK (ADR-0056).
  # The AT-SPI bridge has no macOS counterpart yet, so a11y uses the stub.
  add_languages('objc', required : true)
  window_sources += files(
    'src/app_cocoa.m',
    'src/theme_cocoa.m',
    'src/file_dialog_cocoa.m',
    'src/a11y_stub.c',
    'src/a11y_prefs_cocoa.m',
    'src/platform_wakeup.c',
  )
  window_c_args += '-DOPTICS_WINDOW_BACKEND_COCOA=1'
  window_objc_args += ['-fobjc-arc']
  # AppKit (window/events/IME), QuartzCore (CAMetalLayer for the Vulkan
  # surface), Foundation comes in via Cocoa.
  window_extra_deps += dependency('appleframeworks',
                                modules : ['Cocoa', 'QuartzCore', 'CoreFoundation'])

else
  # ---- Anything else: linkable shell, no backend -------------------------
  # theme_linux.c and file_dialog_portal.c are internally platform-gated
  # (safe defaults / portal-disabled stubs) and compile anywhere; the watch
  # and a11y seams get their stubs.
  window_c_args += '-DOPTICS_WINDOW_BUILD_NO_BACKEND=1'
  window_sources += files(
    'src/theme_linux.c',
    'src/file_dialog_portal.c',
    'src/theme_watch_stub.c',
    'src/a11y_stub.c',
    'src/a11y_prefs_linux.c',
    'src/a11y_prefs_stub.c',
  )
endif

window_lib = library('optics_window',
  window_sources,
  include_directories   : window_inc,
  c_args                : window_c_args,
  objc_args             : window_objc_args,
  dependencies          : [gfx_dep, m_dep] + window_extra_deps,
  gnu_symbol_visibility : 'hidden',
  soversion             : optics_soname,
  version               : meson.project_version(),
  install               : true,
)
install_headers('include/optics/window.h', subdir : 'optics')
install_headers(
    'include/optics/window/loop.h',
  'include/optics/window/capability.h',
  'include/optics/window/cursor.h',
  'include/optics/window/dnd.h',
  'include/optics/window/event.h',
  'include/optics/window/host.h',
  'include/optics/window/theme.h',
  'include/optics/window/file_dialog.h',
  'include/optics/window/a11y.h',
  'include/optics/window/a11y_prefs.h',
  'include/optics/window/state.h',
  subdir : 'optics/window',
)

window_dep = declare_dependency(
  link_with           : window_lib,
  include_directories : window_inc,
  dependencies        : [gfx_dep],
)

pkg.generate(
  libraries   : window_lib,
  name        : 'optics-window',
  filebase    : 'optics-window',
  description : 'Platform shell: native window, event pump and frame cadence over gfx',
  version     : meson.project_version(),
  requires    : ['optics-gfx >= ' + meson.project_version()],
)
