/*
 * iris/iris.h — umbrella header for the L3 application toolkit.
 *
 * Pulls in the cross-platform application API, theme query, file dialog,
 * and the accessibility seam. Per-platform backends are selected at
 * build time (Wayland, Win32, or Cocoa — ADR-0044/0056) by the
 * dispatcher in src/app.c.
 *
 * The surface is intentionally small in v0.1: iris_app_run drives a
 * windowed event loop and dispatches per-frame build callbacks into
 * lens. Theme query reads the system colour scheme at startup and
 * watches it live (portal/libsystemd on Wayland, registry +
 * WM_SETTINGCHANGE on Win32, KVO on Cocoa — ADR-0056). File dialogs use
 * xdg-desktop-portal on Wayland, IFileOpen/SaveDialog on Win32, and
 * NSOpen/SavePanel on Cocoa. The AT-SPI bridge exposes lens's semantic
 * tree to assistive technology on Linux.
 *
 * Architecture and binding decisions:
 *   - iris is the L3 layer of the flux/lens stack (ADR-0043).
 *   - lens owns the widget tree and semantic model; iris owns windows,
 *     event loop, system integration, and the a11y transport.
 *   - Backend selection is compile-time, behind one public signature
 *     (ADR-0044).
 *   - See libs/iris/docs/ for the explanation, ADR, and reference docs.
 */
#ifndef IRIS_H
#define IRIS_H

#include <iris/a11y.h>
#include <iris/a11y_prefs.h>
#include <iris/app.h>
#include <iris/capability.h>
#include <iris/cursor.h>
#include <iris/file_dialog.h>
#include <iris/theme.h>
#include <iris/window.h>

#endif /* IRIS_H */
