/*
 * transit/export.h — single source of truth for symbol visibility.
 *
 * Every public transit header includes this instead of restating the macro
 * (the lens/export.h convention; a drifted export macro is a Windows link
 * error).
 */
#ifndef TRANSIT_EXPORT_H
#define TRANSIT_EXPORT_H

#if defined(_WIN32) && !defined(TRANSIT_STATIC)
#ifdef TRANSIT_BUILDING
#define TRANSIT_API __declspec(dllexport)
#else
#define TRANSIT_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define TRANSIT_API __attribute__((visibility("default")))
#else
#define TRANSIT_API
#endif

#endif /* TRANSIT_EXPORT_H */
