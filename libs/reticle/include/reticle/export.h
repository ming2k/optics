/*
 * reticle/export.h — single source of truth for symbol visibility.
 *
 * Every public reticle header includes this instead of restating the macro
 * (the lens/transit export.h convention; a drifted export macro is a Windows
 * link error).
 */
#ifndef RETICLE_EXPORT_H
#define RETICLE_EXPORT_H

#if defined(_WIN32) && !defined(RETICLE_STATIC)
#ifdef RETICLE_BUILDING
#define RETICLE_API __declspec(dllexport)
#else
#define RETICLE_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define RETICLE_API __attribute__((visibility("default")))
#else
#define RETICLE_API
#endif

#endif /* RETICLE_EXPORT_H */
