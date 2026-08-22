/*
 * lens/export.h — single source of truth for symbol visibility.
 *
 * Every public lens header includes this instead of restating the macro
 * (icon.h once carried a private copy guarded by #ifndef LENS_API; two
 * spellings of one macro drift, and a drifted export macro is a Windows
 * link error). lens.h and icon.h both include this file; neither defines
 * LENS_API itself.
 */
#ifndef LENS_EXPORT_H
#define LENS_EXPORT_H

#if defined(_WIN32) && !defined(LENS_STATIC)
#ifdef LENS_BUILDING
#define LENS_API __declspec(dllexport)
#else
#define LENS_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define LENS_API __attribute__((visibility("default")))
#else
#define LENS_API
#endif

#endif /* LENS_EXPORT_H */
