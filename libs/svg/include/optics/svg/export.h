/*
 * reticle/export.h — single source of truth for symbol visibility.
 *
 * Every public reticle header includes this instead of restating the macro
 * (the lens/transit export.h convention; a drifted export macro is a Windows
 * link error).
 */
#ifndef OPTICS_SVG_EXPORT_H
#define OPTICS_SVG_EXPORT_H

#if defined(_WIN32) && !defined(OPTICS_SVG_STATIC)
#ifdef OPTICS_SVG_BUILDING
#define OPTICS_SVG_API __declspec(dllexport)
#else
#define OPTICS_SVG_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define OPTICS_SVG_API __attribute__((visibility("default")))
#else
#define OPTICS_SVG_API
#endif

#endif /* RETICLE_EXPORT_H */
