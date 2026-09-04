/*
 * prism/types.h — common types and macros for the prism material library.
 */

#ifndef PRISM_TYPES_H
#define PRISM_TYPES_H

#include <flux/core.h>
#include <flux/math.h>

#if defined(_WIN32) && !defined(PRISM_STATIC)
#ifdef PRISM_BUILDING
#define PRISM_API __declspec(dllexport)
#else
#define PRISM_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define PRISM_API __attribute__((visibility("default")))
#else
#define PRISM_API
#endif

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L
#define PRISM_NODISCARD [[nodiscard]]
#elif defined(__GNUC__) || defined(__clang__)
#define PRISM_NODISCARD __attribute__((warn_unused_result))
#elif defined(_MSC_VER)
#define PRISM_NODISCARD _Check_return_
#else
#define PRISM_NODISCARD
#endif

/* PRISM_DEPRECATED(msg): mark a public symbol as scheduled for removal. Mirrors
 * FLUX_DEPRECATED in <flux/core.h>; compiles to the compiler attribute where
 * available so call sites get a warning, and stays empty otherwise so
 * portability is never sacrificed. */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L
#define PRISM_DEPRECATED(msg) [[deprecated(msg)]]
#elif defined(_MSC_VER)
#define PRISM_DEPRECATED(msg) __declspec(deprecated(msg))
#elif defined(__GNUC__) || defined(__clang__)
#define PRISM_DEPRECATED(msg) __attribute__((deprecated(msg)))
#else
#define PRISM_DEPRECATED(msg)
#endif

/* prism release version, kept in lockstep with the stack (see
 * meson.project_version() in the root meson.build). */
#define PRISM_VERSION_MAJOR 0
#define PRISM_VERSION_MINOR 0
#define PRISM_VERSION_PATCH 34

/* Stringify helpers; PRISM_STRINGIFY_ adds the indirection level required
 * for macro-expansion of literal tokens. */
#define PRISM_STRINGIFY_(x) #x
#define PRISM_STRINGIFY(x) PRISM_STRINGIFY_(x)

/* Compile-time version of the library actually linked against, derived from
 * the macros above (not a hardcoded literal). */
PRISM_API const char *prism_version_string(void);

#ifdef __cplusplus
extern "C" {
#endif

/* prism descriptors open with `prism_struct_type type; const void *next;`
 * following the same extension-chain pattern as flux. The registry is
 * prism-local: values are not shared with flux_struct_type. */
typedef enum prism_struct_type {
    PRISM_TYPE_UNKNOWN = 0,
    PRISM_TYPE_LIQUID_GLASS_DESC = 1,
    PRISM_TYPE_FROSTED_DESC = 2,
    PRISM_TYPE_ACRYLIC_DESC = 3,
    PRISM_TYPE_BACKDROP_LAYER_DESC = 4,
    /* Append only. Never repurpose. */
} prism_struct_type;
/* Quality and accessibility degradation hint. */
typedef enum prism_material_quality {
    PRISM_QUALITY_FULL = 0,
    PRISM_QUALITY_LOW_POWER = 1,
    PRISM_QUALITY_OPAQUE_FALLBACK = 2,
} prism_material_quality;

#ifdef __cplusplus
}
#endif

#endif /* PRISM_TYPES_H */
