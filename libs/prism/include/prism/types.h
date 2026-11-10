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
#define PRISM_VERSION_PATCH 76

/* Packed integer version, monotonic — identical layout to
 * FLUX_VERSION_NUMBER (major in bits 16..23, minor 8..15, patch 0..7).
 * The whole stack shares one versioning scheme so a consumer can check
 * every library it loads the same way. */
#define PRISM_VERSION_NUMBER                                                                       \
    (((uint32_t)PRISM_VERSION_MAJOR << 16) | ((uint32_t)PRISM_VERSION_MINOR << 8) |                \
     (uint32_t)PRISM_VERSION_PATCH)

/* Stringify helpers; PRISM_STRINGIFY_ adds the indirection level required
 * for macro-expansion of literal tokens. */
#define PRISM_STRINGIFY_(x) #x
#define PRISM_STRINGIFY(x) PRISM_STRINGIFY_(x)

/* Compile-time version of the library actually linked against, derived from
 * the macros above (not a hardcoded literal). */
PRISM_API void prism_version(int *major, int *minor, int *patch);
PRISM_API uint32_t prism_version_number(void);
/* True when the linked library can stand in for the one compiled
 * against: same major, and its (minor, patch) is >= the requested one.
 * ABI is major-locked; API additions ride minors. */
PRISM_API bool prism_version_check(int major, int minor, int patch);
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
    PRISM_TYPE_MICA_DESC = 5,
    PRISM_TYPE_BACKDROP_LAYER_REVISION = 6,
    /* Append only. Never repurpose. */
} prism_struct_type;
/* Quality and accessibility degradation hint. */
typedef enum prism_material_quality {
    PRISM_QUALITY_FULL = 0,
    PRISM_QUALITY_LOW_POWER = 1,
    PRISM_QUALITY_OPAQUE_FALLBACK = 2,
} prism_material_quality;

/*
 * A material's output: a slab-sized image plus where it belongs.
 *
 * A prism material only paints inside its own footprints, so its output image is
 * sized to those footprints (quantised up a size ladder), NOT to the input
 * capture. That is what keeps a 48-px glass titlebar from allocating three 33 MiB
 * RGBA16F images on a 4K window, and it makes a window resize reuse the same
 * image instead of re-creating one per pixel step.
 *
 * The cost is that `image` no longer covers the input extent, so the caller must
 * place it rather than blit it edge-to-edge:
 *
 *     flux_canvas_draw_image(canvas, out.image,
 *                            (flux_rect){out.origin_x, out.origin_y,
 *                                        (float)flux_image_width(out.image),
 *                                        (float)flux_image_height(out.image)},
 *                            nullptr);
 *
 * The dimensions above are capture pixels; both are whole numbers. `image` is
 * borrowed: it stays valid until the same filter is applied again on the same
 * frame slot, the filter is released, or the required slab extent changes. Do not
 * release it.
 *
 * Everything outside the material's footprints inside the slab is transparent,
 * so drawing the whole slab is always correct — the extra area contributes
 * nothing.
 */
typedef struct prism_material_output {
    flux_image *image;
    float origin_x; /* slab top-left, capture pixels */
    float origin_y;
    uint32_t width; /* slab extent, capture pixels (== the image's extent) */
    uint32_t height;
} prism_material_output;

#ifdef __cplusplus
}
#endif

#endif /* PRISM_TYPES_H */
