/*
 * ui/export.h — single source of truth for symbol visibility.
 *
 * Every public ui header includes this instead of restating the macro
 * (icon.h once carried a private copy guarded by #ifndef OPTICS_UI_API; two
 * spellings of one macro drift, and a drifted export macro is a Windows
 * link error). ui.h and icon.h both include this file; neither defines
 * OPTICS_UI_API itself.
 */
#ifndef OPTICS_UI_EXPORT_H
#define OPTICS_UI_EXPORT_H

#if defined(_WIN32) && !defined(OPTICS_UI_STATIC)
#ifdef OPTICS_UI_BUILDING
#define OPTICS_UI_API __declspec(dllexport)
#else
#define OPTICS_UI_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define OPTICS_UI_API __attribute__((visibility("default")))
#else
#define OPTICS_UI_API
#endif

/* OPTICS_UI_NODISCARD: mark a function whose return value must be checked. */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L
#define OPTICS_UI_NODISCARD [[nodiscard]]
#elif defined(__GNUC__) || defined(__clang__)
#define OPTICS_UI_NODISCARD __attribute__((warn_unused_result))
#elif defined(_MSC_VER)
#define OPTICS_UI_NODISCARD _Check_return_
#else
#define OPTICS_UI_NODISCARD
#endif

/* OPTICS_UI_INLINE: header-only inline helpers (never combined with dllexport/dllimport). */
#define OPTICS_UI_INLINE static inline

#endif /* OPTICS_UI_EXPORT_H */
