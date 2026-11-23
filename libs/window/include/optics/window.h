/*
 * window/window.h — umbrella header for the L3 application toolkit.
 *
 * Pulls in the cross-platform application API, theme query, file dialog,
 * and the accessibility seam. Per-platform backends are selected at
 * build time (Wayland, Win32, or Cocoa — ADR-0044/0056) by the
 * dispatcher in src/app.c.
 *
 * The surface is intentionally small in v0.1: optics_window_run drives a
 * windowed event loop and dispatches per-frame build callbacks into
 * ui. Theme query reads the system colour scheme at startup and
 * watches it live (portal/libsystemd on Wayland, registry +
 * WM_SETTINGCHANGE on Win32, KVO on Cocoa — ADR-0056). File dialogs use
 * xdg-desktop-portal on Wayland, IFileOpen/SaveDialog on Win32, and
 * NSOpen/SavePanel on Cocoa. The AT-SPI bridge exposes ui's semantic
 * tree to assistive technology on Linux.
 *
 * Architecture and binding decisions:
 *   - window is the L3 layer of the gfx/ui stack (ADR-0043).
 *   - ui owns the widget tree and semantic model; window owns windows,
 *     event loop, system integration, and the a11y transport.
 *   - Backend selection is compile-time, behind one public signature
 *     (ADR-0044).
 *   - See libs/window/docs/ for the explanation, ADR, and reference docs.
 */
#ifndef OPTICS_WINDOW_H
#define OPTICS_WINDOW_H

#include <optics/window/a11y.h>
#include <optics/window/a11y_prefs.h>
#include <optics/window/capability.h>
#include <optics/window/cursor.h>
#include <optics/window/dnd.h>
#include <optics/window/event.h>
#include <optics/window/file_dialog.h>
#include <optics/window/host.h>
#include <optics/window/loop.h>
#include <optics/window/state.h>
#include <optics/window/theme.h>

#endif /* OPTICS_WINDOW_H */
