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
