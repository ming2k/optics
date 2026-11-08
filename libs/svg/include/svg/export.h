/*
 * svg/export.h — single source of truth for symbol visibility.
 *
 * Every public svg header includes this instead of restating the macro
 * (the lens/transit export.h convention; a drifted export macro is a Windows
 * link error).
 */
#ifndef SVG_EXPORT_H
#define SVG_EXPORT_H

#if defined(_WIN32) && !defined(SVG_STATIC)
#ifdef SVG_BUILDING
#define SVG_API __declspec(dllexport)
#else
#define SVG_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define SVG_API __attribute__((visibility("default")))
#else
#define SVG_API
#endif

#endif /* SVG_EXPORT_H */
