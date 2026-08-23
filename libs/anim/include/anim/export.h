/*
 * anim/export.h — single source of truth for symbol visibility.
 *
 * Every public anim header includes this instead of restating the macro
 * (the lens/export.h convention; a drifted export macro is a Windows link
 * error).
 */
#ifndef ANIM_EXPORT_H
#define ANIM_EXPORT_H

#if defined(_WIN32) && !defined(ANIM_STATIC)
#ifdef ANIM_BUILDING
#define ANIM_API __declspec(dllexport)
#else
#define ANIM_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define ANIM_API __attribute__((visibility("default")))
#else
#define ANIM_API
#endif

#endif /* ANIM_EXPORT_H */
