/* platform_internal.h — portable platform interface for iris.
 *
 * Defines the contract between an application and the platform backend
 * that owns the window, GPU device, and event loop.  The current
 * Wayland implementation (app_wayland.c) satisfies this interface;
 * future ports (Win32, Cocoa) implement the same functions and types.
 *
 * An application calls iris_app_run_wayland(); the platform calls the
 * application's build + paint callbacks each frame.
 */
#ifndef IRIS_PLATFORM_INTERNAL_H
#define IRIS_PLATFORM_INTERNAL_H

#include "iris/app.h"

int iris_app_run_wayland(const iris_app_config *cfg);

#endif /* IRIS_PLATFORM_INTERNAL_H */
