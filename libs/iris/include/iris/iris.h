/*
 * iris/iris.h — umbrella header for the L3 application toolkit.
 *
 * Pulls in the cross-platform application API, theme query, file dialog,
 * and the accessibility seam. Per-platform backends are selected at
 * build time (today: Wayland only; future: Win32, Cocoa) by the
 * dispatcher in src/app.c.
 *
 * The surface is intentionally small in v0.1: iris_app_run drives a
 * windowed event loop and dispatches per-frame build callbacks into
 * lens. Theme query reads the system colour scheme at startup and
 * watches it live when libsystemd is available. File dialog shells out
 * to xdg-desktop-portal. The AT-SPI bridge exposes lens's semantic
 * tree to assistive technology.
 *
 * Architecture and binding decisions:
 *   - iris is the L3 layer of the flux/lens stack (ADR-0001).
 *   - lens owns the widget tree and semantic model; iris owns windows,
 *     event loop, system integration, and the a11y transport.
 *   - Backend selection is compile-time, behind one public signature
 *     (ADR-0005).
 *   - See libs/iris/docs/ for the explanation, ADR, and reference docs.
 */
#ifndef IRIS_H
#define IRIS_H

#include <iris/a11y.h>
#include <iris/app.h>
#include <iris/cursor.h>
#include <iris/file_dialog.h>
#include <iris/theme.h>

#endif /* IRIS_H */
