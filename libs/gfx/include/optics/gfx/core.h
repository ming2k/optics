/*
 * flux/core.h — device, surface, frame, allocator, logger, results.
 *
 * Pure C. No <vulkan/vulkan.h> in this header. Pull <flux/vulkan.h>
 * at the seam where you hand flux a VkSurfaceKHR or pull a raw
 * VkCommandBuffer back.
 *
 * Design contract:
 *   - Opaque handles. Never dereference flux_device, flux_surface,
 *     flux_frame, flux_image. They are forward declarations.
 *   - Refcount the heavy (device, surface, GPU resources);
 *     value types live on the caller's stack or in an arena.
 *   - Every fallible call returns flux_result and is marked
 *     [[nodiscard]].
 *   - Descriptor structs open with `flux_struct_type type;
 *     const void *next;` for extension chains (Vulkan sType/pNext
 *     pattern).
 */

#ifndef OPTICS_GFX_CORE_H
#define OPTICS_GFX_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A rectangle in surface-local physical pixels. Kept in core.h because damage
 * is a first-class frame input/output, not a canvas-private detail. The math
 * helpers remain in <flux/math.h>. */
typedef struct optics_gfx_rect {
    float x, y, w, h;
} optics_gfx_rect;

/* ================================================================== */
/*  Visibility & attributes                                           */
/* ================================================================== */

#if defined(_WIN32) && !defined(OPTICS_GFX_STATIC)
#ifdef OPTICS_GFX_BUILDING
#define OPTICS_GFX_API __declspec(dllexport)
#else
#define OPTICS_GFX_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define OPTICS_GFX_API __attribute__((visibility("default")))
#else
#define OPTICS_GFX_API
#endif

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L
#define OPTICS_GFX_NODISCARD [[nodiscard]]
#elif defined(__GNUC__) || defined(__clang__)
#define OPTICS_GFX_NODISCARD __attribute__((warn_unused_result))
#elif defined(_MSC_VER)
#define OPTICS_GFX_NODISCARD _Check_return_
#else
#define OPTICS_GFX_NODISCARD
#endif

/* ================================================================== */
/*  Versioning                                                        */
/* ================================================================== */

#define OPTICS_GFX_VERSION_MAJOR 0
#define OPTICS_GFX_VERSION_MINOR 0
#define OPTICS_GFX_VERSION_PATCH 82

/* Packed integer version, monotonic: bits 16..23 major, 8..15 minor,
 * 0..7 patch. 0.0.1 == 0x00000001; 1.2.3 == 0x00010203. */
#define OPTICS_GFX_VERSION_NUMBER                                                                  \
    (((uint32_t)OPTICS_GFX_VERSION_MAJOR << 16) | ((uint32_t)OPTICS_GFX_VERSION_MINOR << 8) |      \
     (uint32_t)OPTICS_GFX_VERSION_PATCH)

OPTICS_GFX_API void optics_gfx_version(int *major, int *minor, int *patch);
OPTICS_GFX_API uint32_t optics_gfx_version_number(void);
OPTICS_GFX_API bool optics_gfx_version_check(int major, int minor, int patch);
OPTICS_GFX_API const char *optics_gfx_version_string(void);

/* ================================================================== */
/*  Result codes                                                      */
/* ================================================================== */

typedef enum optics_gfx_result : uint32_t {
    OPTICS_GFX_OK = 0,
    OPTICS_GFX_ERROR_INVALID_ARGUMENT = 1,
    OPTICS_GFX_ERROR_OUT_OF_MEMORY = 2,
    OPTICS_GFX_ERROR_OUT_OF_RANGE = 3,
    OPTICS_GFX_ERROR_INVALID_STATE = 4,
    OPTICS_GFX_ERROR_UNSUPPORTED = 5,
    OPTICS_GFX_ERROR_BACKEND_FAILURE = 6,
    OPTICS_GFX_ERROR_DEVICE_LOST = 7,
    OPTICS_GFX_ERROR_SURFACE_LOST = 8,
    OPTICS_GFX_ERROR_TIMEOUT = 9,
    /* The surface ring slot cannot be reused yet: an exported image lease is
     * still held by an external consumer. */
    OPTICS_GFX_ERROR_BUSY = 10,
} optics_gfx_result;

OPTICS_GFX_NODISCARD OPTICS_GFX_API const char *optics_gfx_result_string(optics_gfx_result r);

/* Structured diagnostic for the most recent error on this thread.
 *
 * Thread-local: each thread keeps its own slot. A non-OK return on
 * thread A does not affect what thread B observes.
 *
 * The string fields are owned by flux (typically literals or static
 * buffers); never free them. The strings remain valid until the next
 * non-OK return on the same thread, after which they may be
 * overwritten in place. Any pointer may be NULL.
 *
 * Call after a non-OK return for context. */
typedef struct optics_gfx_error_info {
    optics_gfx_result code;
    const char *function; /* "flux_device_create" */
    const char *file;     /* source file inside flux */
    int line;
    const char *message;  /* human-readable detail; may be NULL */
    int32_t backend_code; /* e.g. VkResult cast to int when relevant */
} optics_gfx_error_info;

/* Copies the most recent thread-local diagnostic into *out. The
 * `out` struct becomes the caller's; the string pointers inside it
 * follow the lifetime rules documented on flux_error_info above. */
OPTICS_GFX_API void optics_gfx_get_last_error(optics_gfx_error_info *out);

/* Record a diagnostic for a failure a *consumer library* detected.
 *
 * Flux owns the thread-local slot, but validation does not stop at flux's
 * boundary: lens rejects a snapshot activation for a stale generation, a
 * mismatched owner, an in-progress build — failures flux never sees, with the
 * same result codes. Without this, a binding reading the slot after such a
 * failure gets whatever the *previous* flux call left there: a diagnostic for a
 * different function, or none at all.
 *
 * Call it immediately before returning the non-OK code, so the slot describes
 * this failure at the moment the caller can still read it. `function` and
 * `message` must outlive the slot's contents (a literal, or `flux_error_format`
 * for composed text); file/line point at the rejecting site.
 *
 * `function` is the *entry point the caller invoked*, not this helper — e.g.
 * "lens_snapshot_activate" — because that is what the caller's log needs to name.
 * The slot is thread-local, matching flux's own failures: one thread's
 * diagnostic never affects another's. */
OPTICS_GFX_API void optics_gfx_set_diagnostic(const optics_gfx_error_info *info);

/* Compose a message into the thread-local formatting arena and return it, for
 * passing to `flux_set_diagnostic` when the text needs values spliced in. The
 * returned pointer is valid until the next `flux_set_diagnostic` on this thread.
 * Truncates rather than failing: a diagnostic must never be the reason a call
 * fails differently. */
[[nodiscard]] OPTICS_GFX_API const char *optics_gfx_diagnostic_format(const char *fmt, ...);

/* ================================================================== */
/*  Format                                                            */
/* ================================================================== */

/* Backend-neutral pixel/attachment format used by canvas images and
 * scene materials. Mapped to VkFormat by flux_format_to_vk
 * (declared in <flux/vulkan.h>). The UNORM/SRGB distinction follows
 * Vulkan: UNORM writes data verbatim; SRGB applies the sRGB
 * transfer function on store. */
typedef enum optics_gfx_format : uint32_t {
    OPTICS_GFX_FORMAT_UNDEFINED = 0,

    /* 8-bit colour */
    OPTICS_GFX_FORMAT_R8_UNORM = 1,
    OPTICS_GFX_FORMAT_RGBA8_UNORM = 2,
    OPTICS_GFX_FORMAT_BGRA8_UNORM = 3,
    OPTICS_GFX_FORMAT_RGBA8_SRGB = 4,
    OPTICS_GFX_FORMAT_BGRA8_SRGB = 5,

    /* 10-bit colour (packed A2B10G10R10; DRM_FORMAT_ABGR2101010) */
    OPTICS_GFX_FORMAT_RGB10A2_UNORM = 14,

    /* HDR / float colour */
    OPTICS_GFX_FORMAT_RGBA16_SFLOAT = 6,

    /* Depth */
    OPTICS_GFX_FORMAT_D32_SFLOAT = 7,
    OPTICS_GFX_FORMAT_D24_UNORM_S8 = 8,
    OPTICS_GFX_FORMAT_D32_SFLOAT_S8 = 9,

    /* Float vertex attributes */
    OPTICS_GFX_FORMAT_R32_SFLOAT = 10,
    OPTICS_GFX_FORMAT_RG32_SFLOAT = 11,
    OPTICS_GFX_FORMAT_RGB32_SFLOAT = 12,
    OPTICS_GFX_FORMAT_RGBA32_SFLOAT = 13,
} optics_gfx_format;

/* ================================================================== */
/*  Color space (ADR-0069)                                            */
/* ================================================================== */

/* A color space is { primaries, transfer function } — the parametric
 * model VkColorSpaceKHR, DXGI, the Wayland color-management protocol
 * and CSS Color 4 have converged on. There is deliberately no range
 * field: flux renders RGB only; video-range YUV belongs to the decoder
 * that hands flux RGB content.
 *
 * All named primaries are D65; custom primaries carry their own white
 * point and conversions adapt chromatically (Bradford). */

typedef enum optics_gfx_color_primaries : uint32_t {
    OPTICS_GFX_PRIMARIES_BT709 = 0,      /* sRGB gamut, D65 */
    OPTICS_GFX_PRIMARIES_DISPLAY_P3 = 1, /* D65 */
    OPTICS_GFX_PRIMARIES_BT2020 = 2,     /* D65 */
    OPTICS_GFX_PRIMARIES_ADOBE_RGB = 3,  /* D65 */
    OPTICS_GFX_PRIMARIES_CUSTOM = 4,     /* chromaticities from flux_color_space.xy */
} optics_gfx_color_primaries;

/* Transfer function between linear-light and encoded values. PQ and
 * HLG are absolute/nominal HDR curves (ITU-R BT.2100): PQ linear
 * values are in units of 10000 cd/m² (1.0 = 10000 nits); HLG is
 * nominal 0..1 scene light. The working-space luminance scale follows
 * scRGB: 1.0 = 80 cd/m². */
typedef enum optics_gfx_transfer_func : uint32_t {
    OPTICS_GFX_TRANSFER_LINEAR = 0,
    OPTICS_GFX_TRANSFER_SRGB = 1,
    OPTICS_GFX_TRANSFER_GAMMA = 2, /* pure power law, exponent from flux_color_space.gamma */
    OPTICS_GFX_TRANSFER_PQ = 3,    /* ST 2084 */
    OPTICS_GFX_TRANSFER_HLG = 4,
} optics_gfx_transfer_func;

typedef struct optics_gfx_color_space {
    optics_gfx_color_primaries primaries;
    optics_gfx_transfer_func transfer;
    float gamma; /* FLUX_TRANSFER_GAMMA only; 0 otherwise */
    struct {
        float rx, ry, gx, gy, bx, by;
        float wx, wy; /* white point */
    } xy;             /* FLUX_PRIMARIES_CUSTOM only; zero otherwise */
} optics_gfx_color_space;

/* Brace-initializer presets (same convention as FLUX_SURFACE_DESC_INIT).
 * The set follows CSS Color 4 plus the HDR surface spaces. */
#define OPTICS_GFX_COLOR_SPACE_SRGB                                                                \
    {                                                                                              \
        OPTICS_GFX_PRIMARIES_BT709, OPTICS_GFX_TRANSFER_SRGB, 0.0f, {}                             \
    }
#define OPTICS_GFX_COLOR_SPACE_SRGB_LINEAR                                                         \
    {                                                                                              \
        OPTICS_GFX_PRIMARIES_BT709, OPTICS_GFX_TRANSFER_LINEAR, 0.0f, {}                           \
    }
/* scRGB: the fixed working space (extended linear BT.709, 1.0 = 80 nits).
 * Same {primaries, transfer} as SRGB_LINEAR; the distinct preset names the
 * extended-range surface intent (VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT). */
#define OPTICS_GFX_COLOR_SPACE_SCRGB                                                               \
    {                                                                                              \
        OPTICS_GFX_PRIMARIES_BT709, OPTICS_GFX_TRANSFER_LINEAR, 0.0f, {}                           \
    }
#define OPTICS_GFX_COLOR_SPACE_DISPLAY_P3                                                          \
    {                                                                                              \
        OPTICS_GFX_PRIMARIES_DISPLAY_P3, OPTICS_GFX_TRANSFER_SRGB, 0.0f, {}                        \
    }
#define OPTICS_GFX_COLOR_SPACE_DISPLAY_P3_LINEAR                                                   \
    {                                                                                              \
        OPTICS_GFX_PRIMARIES_DISPLAY_P3, OPTICS_GFX_TRANSFER_LINEAR, 0.0f, {}                      \
    }
/* Adobe RGB (1998): the official profile's TRC is a pure 563/256 power. */
#define OPTICS_GFX_COLOR_SPACE_ADOBE_RGB                                                           \
    {                                                                                              \
        OPTICS_GFX_PRIMARIES_ADOBE_RGB, OPTICS_GFX_TRANSFER_GAMMA, 2.19921875f, {}                 \
    }
/* SDR BT.2020 per BT.1886 practice: pure 2.4 power. */
#define OPTICS_GFX_COLOR_SPACE_BT2020                                                              \
    {                                                                                              \
        OPTICS_GFX_PRIMARIES_BT2020, OPTICS_GFX_TRANSFER_GAMMA, 2.4f, {}                           \
    }
/* HDR10. */
#define OPTICS_GFX_COLOR_SPACE_BT2020_PQ                                                           \
    {                                                                                              \
        OPTICS_GFX_PRIMARIES_BT2020, OPTICS_GFX_TRANSFER_PQ, 0.0f, {}                              \
    }
#define OPTICS_GFX_COLOR_SPACE_BT2020_HLG                                                          \
    {                                                                                              \
        OPTICS_GFX_PRIMARIES_BT2020, OPTICS_GFX_TRANSFER_HLG, 0.0f, {}                             \
    }

/* ================================================================== */
/*  Tagged-struct discriminator                                       */
/* ================================================================== */

typedef enum optics_gfx_struct_type : uint32_t {
    OPTICS_GFX_TYPE_UNKNOWN = 0,
    OPTICS_GFX_TYPE_DEVICE_DESC = 1,
    OPTICS_GFX_TYPE_SURFACE_DESC = 2,
    OPTICS_GFX_TYPE_FRAME_BEGIN_DESC = 3,
    OPTICS_GFX_TYPE_PASS_DESC = 4,
    OPTICS_GFX_TYPE_IMAGE_DESC = 5,
    OPTICS_GFX_TYPE_CANVAS_DESC = 6,
    OPTICS_GFX_TYPE_MESH_DESC = 7,
    OPTICS_GFX_TYPE_MATERIAL_DESC = 8,
    OPTICS_GFX_TYPE_COMPUTE_PIPELINE_DESC = 9,
    OPTICS_GFX_TYPE_SAMPLER_DESC = 10,
    OPTICS_GFX_TYPE_BUFFER_DESC = 11,
    OPTICS_GFX_TYPE_GRAPHICS_PIPELINE_DESC = 12,
    OPTICS_GFX_TYPE_EFFECT_BLUR_DESC = 13,
    OPTICS_GFX_TYPE_DMABUF_IMAGE_DESC = 14,
    OPTICS_GFX_TYPE_GLYPH_RUN_DESC = 15,
    OPTICS_GFX_TYPE_TARGET_DESC = 16,
    OPTICS_GFX_TYPE_SURFACE_DMABUF_DESC = 17,
    OPTICS_GFX_TYPE_SURFACE_READBACK_DESC = 18,
    OPTICS_GFX_TYPE_CANVAS_PASS_DESC = 19,
    /* 20 was the liquid-glass material's descriptor tag — retired when the
     * material moved to the prism library (ADR-0063). Never reuse. */
    OPTICS_GFX_TYPE_MESH_SKIN_DESC = 21,
    OPTICS_GFX_TYPE_MATERIAL_SURFACE_DESC = 22,
    OPTICS_GFX_TYPE_EFFECT_BLUR_REGIONS_DESC = 23,
    /* 24 was FLUX_TYPE_CANVAS_NO_STENCIL_DESC — retired when analytic
     * tiled coverage replaced stencil-then-cover (ADR-0116/0120). Never reuse. */
    OPTICS_GFX_TYPE_DEVICE_DRM_NODE_DESC = 25,
    OPTICS_GFX_TYPE_DEVICE_FEATURES_DESC = 26,
    OPTICS_GFX_TYPE_GLYPH_RUN_HOST_ATLAS_DESC = 27,
    OPTICS_GFX_TYPE_SURFACE_COLOR_SPACE_DESC = 28,
    OPTICS_GFX_TYPE_SURFACE_HDR_DESC = 29,
    OPTICS_GFX_TYPE_IMAGE_COLOR_SPACE_DESC = 30,
    OPTICS_GFX_TYPE_SURFACE_OUTPUT_COLOR_DESC = 31,
    OPTICS_GFX_TYPE_SURFACE_OFFSCREEN_FORMAT_DESC = 32,
    OPTICS_GFX_TYPE_EFFECT_SHADOW_DESC = 40,
    OPTICS_GFX_TYPE_CPU_TARGET_DESC = 46,
    OPTICS_GFX_TYPE_MATERIAL_VECTOR_DESC = 47,
    OPTICS_GFX_TYPE_OBSERVE_OPTS = 48,
    OPTICS_GFX_TYPE_OBSERVE_REPORT_OPTS = 49,
    OPTICS_GFX_TYPE_DEVICE_ALLOC_OPTS = 50,
    OPTICS_GFX_TYPE_DEVICE_FREE_OPTS = 51,
    OPTICS_GFX_TYPE_DEFERRED_PASS_DESC = 52,
    OPTICS_GFX_TYPE_DISPLAY_LIST_SERIALIZE_OPTS = 53,
    OPTICS_GFX_TYPE_DISPLAY_LIST_DESERIALIZE_OPTS = 54,
    /* Append only. Never repurpose. */
} optics_gfx_struct_type;

/* Compile-time ABI drift guards. These values are wire/ABI identifiers:
 * renumbering one silently mis-routes every chained descriptor, and
 * nothing else would catch it (the library validates `type` at runtime,
 * so a drifted header fails far from the cause). If any of these asserts
 * fires, a value was inserted or reordered instead of appended — see
 * docs/reference/api.md before changing them. */
static_assert(OPTICS_GFX_TYPE_DEVICE_DESC == 1, "flux_struct_type: append only — never renumber");
static_assert(OPTICS_GFX_TYPE_EFFECT_SHADOW_DESC == 40,
              "flux_struct_type: append only — never renumber");
static_assert(OPTICS_GFX_TYPE_OBSERVE_OPTS == 48, "flux_struct_type: append only — never renumber");
static_assert(OPTICS_GFX_TYPE_OBSERVE_REPORT_OPTS == 49,
              "flux_struct_type: append only — never renumber");
static_assert(OPTICS_GFX_TYPE_DEVICE_ALLOC_OPTS == 50,
              "flux_struct_type: append only — never renumber");
static_assert(OPTICS_GFX_TYPE_DEVICE_FREE_OPTS == 51,
              "flux_struct_type: append only — never renumber");
static_assert(OPTICS_GFX_TYPE_DEFERRED_PASS_DESC == 52,
              "flux_struct_type: append only — never renumber");
static_assert(OPTICS_GFX_TYPE_DISPLAY_LIST_SERIALIZE_OPTS == 53,
              "flux_struct_type: append only — never renumber");
static_assert(OPTICS_GFX_TYPE_DISPLAY_LIST_DESERIALIZE_OPTS == 54,
              "flux_struct_type: append only — never renumber");

/* ================================================================== */
/*  C23 Type-Safe Compound Literal Initializer (RFC-0094 / ADR-0087)  */
/* ================================================================== */

#define OPTICS_GFX_INIT(TypeName, ...)                                                             \
    ((optics_gfx_##TypeName){.type = OPTICS_GFX_TYPE_##TypeName, .next = nullptr, __VA_ARGS__})

#define OPTICS_GFX_TYPE_device_desc OPTICS_GFX_TYPE_DEVICE_DESC
#define OPTICS_GFX_TYPE_surface_desc OPTICS_GFX_TYPE_SURFACE_DESC
#define OPTICS_GFX_TYPE_frame_begin_desc OPTICS_GFX_TYPE_FRAME_BEGIN_DESC
#define OPTICS_GFX_TYPE_pass_desc OPTICS_GFX_TYPE_PASS_DESC
#define OPTICS_GFX_TYPE_image_desc OPTICS_GFX_TYPE_IMAGE_DESC
#define OPTICS_GFX_TYPE_canvas_desc OPTICS_GFX_TYPE_CANVAS_DESC
#define OPTICS_GFX_TYPE_mesh_desc OPTICS_GFX_TYPE_MESH_DESC
#define OPTICS_GFX_TYPE_material_desc OPTICS_GFX_TYPE_MATERIAL_DESC
#define OPTICS_GFX_TYPE_material_vector_desc OPTICS_GFX_TYPE_MATERIAL_VECTOR_DESC
#define OPTICS_GFX_TYPE_compute_pipeline_desc OPTICS_GFX_TYPE_COMPUTE_PIPELINE_DESC
#define OPTICS_GFX_TYPE_sampler_desc OPTICS_GFX_TYPE_SAMPLER_DESC
#define OPTICS_GFX_TYPE_buffer_desc OPTICS_GFX_TYPE_BUFFER_DESC
#define OPTICS_GFX_TYPE_graphics_pipeline_desc OPTICS_GFX_TYPE_GRAPHICS_PIPELINE_DESC
#define OPTICS_GFX_TYPE_effect_blur_desc OPTICS_GFX_TYPE_EFFECT_BLUR_DESC
#define OPTICS_GFX_TYPE_target_desc OPTICS_GFX_TYPE_TARGET_DESC
#define OPTICS_GFX_TYPE_cpu_target_desc OPTICS_GFX_TYPE_CPU_TARGET_DESC
#define OPTICS_GFX_TYPE_canvas_pass_desc OPTICS_GFX_TYPE_CANVAS_PASS_DESC
#define OPTICS_GFX_TYPE_glyph_run_desc OPTICS_GFX_TYPE_GLYPH_RUN_DESC
#define OPTICS_GFX_TYPE_observe_opts OPTICS_GFX_TYPE_OBSERVE_OPTS
#define OPTICS_GFX_TYPE_observe_report_opts OPTICS_GFX_TYPE_OBSERVE_REPORT_OPTS
#define OPTICS_GFX_TYPE_device_alloc_opts OPTICS_GFX_TYPE_DEVICE_ALLOC_OPTS
#define OPTICS_GFX_TYPE_device_free_opts OPTICS_GFX_TYPE_DEVICE_FREE_OPTS
#define OPTICS_GFX_TYPE_display_list_serialize_opts OPTICS_GFX_TYPE_DISPLAY_LIST_SERIALIZE_OPTS
#define OPTICS_GFX_TYPE_display_list_deserialize_opts OPTICS_GFX_TYPE_DISPLAY_LIST_DESERIALIZE_OPTS

typedef struct optics_gfx_device_desc optics_gfx_DEVICE_DESC;
typedef struct optics_gfx_surface_desc optics_gfx_SURFACE_DESC;
typedef struct optics_gfx_frame_begin_desc optics_gfx_FRAME_BEGIN_DESC;
typedef struct optics_gfx_pass_desc optics_gfx_PASS_DESC;
typedef struct optics_gfx_image_desc optics_gfx_IMAGE_DESC;
typedef struct optics_gfx_canvas_desc optics_gfx_CANVAS_DESC;
typedef struct optics_gfx_mesh_desc optics_gfx_MESH_DESC;
typedef struct optics_gfx_material_desc optics_gfx_MATERIAL_DESC;
typedef struct optics_gfx_compute_pipeline_desc optics_gfx_COMPUTE_PIPELINE_DESC;
typedef struct optics_gfx_sampler_desc optics_gfx_SAMPLER_DESC;
typedef struct optics_gfx_buffer_desc optics_gfx_BUFFER_DESC;
typedef struct optics_gfx_graphics_pipeline_desc optics_gfx_GRAPHICS_PIPELINE_DESC;
typedef struct optics_gfx_effect_blur_desc optics_gfx_EFFECT_BLUR_DESC;
typedef struct optics_gfx_target_desc optics_gfx_TARGET_DESC;
typedef struct optics_gfx_cpu_target_desc optics_gfx_CPU_TARGET_DESC;
typedef struct optics_gfx_canvas_pass_desc optics_gfx_CANVAS_PASS_DESC;
typedef struct optics_gfx_glyph_run_desc optics_gfx_GLYPH_RUN_DESC;

/* ================================================================== */
/*  Allocation scope (ADR-0129; declared here because core.h owns the
    allocation entry points that take it)                              */
/* ================================================================== */

/* Where an acquisition belongs. The scope decides *when* a non-zero live count
 * becomes a defect, not how it is counted. */
typedef enum optics_gfx_scope : uint32_t {
    /* flux GPU and host objects. Drain point: device teardown, after the
     * retire queue has drained (so in-flight work is never reported). */
    OPTICS_GFX_SCOPE_DEVICE = 0,
    /* Per-context state above flux: lens store, glyph atlas, vista scene,
     * prism material caches. Drain point: the owning context's release. */
    OPTICS_GFX_SCOPE_CONTEXT = 1,
    /* Process-global OS objects: file descriptors, Wayland protocol objects,
     * VkSurfaceKHR. Process-global ledger; pass nullptr as the device.
     * Drain point: process exit. */
    OPTICS_GFX_SCOPE_PROCESS = 2,
    OPTICS_GFX_SCOPE_COUNT = 3,
} optics_gfx_scope;

/* ================================================================== */
/*  Allocator & logger (caller-supplied)                              */
/* ================================================================== */

/* Caller-supplied host allocator.
 *
 * `realloc` was removed: no library ever called it, so its semantics were
 * undefined by construction and it was a trap in a public struct. Growth paths
 * allocate-copy-free explicitly, which the ledger can observe (ADR-0135).
 *
 * zero-init = libc malloc/free. */
typedef struct optics_gfx_allocator {
    void *(*alloc)(size_t bytes, void *user);
    void (*free)(void *ptr, void *user);
    void *user;
} optics_gfx_allocator;

typedef enum optics_gfx_log_level : uint32_t {
    OPTICS_GFX_LOG_TRACE = 0,
    OPTICS_GFX_LOG_DEBUG = 1,
    OPTICS_GFX_LOG_INFO = 2,
    OPTICS_GFX_LOG_WARN = 3,
    OPTICS_GFX_LOG_ERROR = 4,
} optics_gfx_log_level;

typedef void (*optics_gfx_log_fn)(optics_gfx_log_level level, const char *file, int line,
                                  const char *fmt, const char *msg, void *user);

OPTICS_GFX_API void optics_gfx_console_logger(optics_gfx_log_level level, const char *file,
                                              int line, const char *fmt, const char *msg,
                                              void *user);

/* Forward declarations — needed early so callback signatures and
 * function prototypes below can reference the opaque handles. */
typedef struct optics_gfx_device optics_gfx_device;
typedef struct optics_gfx_surface optics_gfx_surface;
typedef struct optics_gfx_frame optics_gfx_frame;
typedef struct optics_gfx_readback optics_gfx_readback;
typedef struct optics_gfx_buffer optics_gfx_buffer;
typedef struct optics_gfx_target optics_gfx_target;
typedef struct optics_gfx_image optics_gfx_image;
typedef struct optics_gfx_icc_profile optics_gfx_icc_profile;

/* ------------------------------------------------------------------ */
/*  ICC profile (device-free parsed transform source, ADR-0070)       */
/* ------------------------------------------------------------------ */

/* Parses and validates an ICC v2/v4 display/scanner-class RGB profile.
 * The parser implements the bounded subset flux consumes: matrix +
 * TRC profiles (curveType gamma, parametricCurveType 0-4) and A2B0
 * LUT profiles (mft1/mft2/mAB); anything else is
 * FLUX_ERROR_UNSUPPORTED. Parsing never touches the GPU. */
OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result
optics_gfx_icc_profile_create(const void *data, size_t size, optics_gfx_icc_profile **out);
OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_icc_profile *
optics_gfx_icc_profile_retain(optics_gfx_icc_profile *p);
OPTICS_GFX_API void optics_gfx_icc_profile_release(optics_gfx_icc_profile *p);

/* When the profile is exactly representable in the parametric model
 * (matrix + a transfer curve from the flux set), fills `out` and
 * returns true. LUT-only profiles return false; they are still usable
 * on images, where they are baked into a 3D LUT at creation. */
OPTICS_GFX_API bool optics_gfx_icc_profile_color_space(const optics_gfx_icc_profile *p,
                                                       optics_gfx_color_space *out);

/* ------------------------------------------------------------------ */
/*  Sampled image (refcounted GPU resource)                           */
/* ------------------------------------------------------------------ */

/* What an image is for. One vocabulary for creation (flux_image_desc::usage)
 * and for the capability query below. */
typedef enum optics_gfx_image_usage : uint32_t {
    /* Sampled content, uploaded from `initial_data` or by region updates. */
    OPTICS_GFX_IMAGE_USAGE_SAMPLED = 0,
    /* STORAGE | SAMPLED | TRANSFER, parked in SHADER_READ_ONLY_OPTIMAL: the
     * intermediate currency of compute effects. Bring it to GENERAL around a
     * dispatch with flux_image_transition (<flux/vulkan.h>). RGBA8/BGRA8 UNORM
     * always; RGBA16_SFLOAT where the device supports rgba16f storage. Holds
     * only a weak device reference: keep the device alive for its lifetime. */
    OPTICS_GFX_IMAGE_USAGE_COMPUTE_WRITE = 1,
    /* COLOR_ATTACHMENT | SAMPLED with undefined initial contents. Prepare it
     * before flux_frame_begin_pass and finish it after flux_frame_end_pass;
     * outside that interval it is sampleable. */
    OPTICS_GFX_IMAGE_USAGE_RENDER_TARGET = 2,
} optics_gfx_image_usage;

/* Images are core GPU resources shared by canvas, scene, and effect
 * modules. Pixel uploads are tightly packed and copied before create/update
 * returns. Render-target helpers keep the image sampleable between passes. */
typedef struct optics_gfx_image_desc {
    optics_gfx_struct_type type; /* FLUX_TYPE_IMAGE_DESC */
    const void *next;
    uint32_t width;
    uint32_t height;
    optics_gfx_format format;     /* 8-bit colour formats, or RGBA16_SFLOAT
                                   * (working-space linear content, ADR-0069) */
    const void *initial_data;     /* SAMPLED only, optional; size = w*h*bytes_per_pixel */
    optics_gfx_image_usage usage; /* default SAMPLED */
} optics_gfx_image_desc;

#define OPTICS_GFX_IMAGE_DESC_INIT {.type = OPTICS_GFX_TYPE_IMAGE_DESC}

/* Optional flux_image_desc extension tagging the content's color space
 * (ADR-0069/0070). Untagged images keep the format-derived default:
 * 8-bit formats are sRGB, RGBA16_SFLOAT is working-space linear.
 *
 * `space` tags parametric content (e.g. a Display P3 texture). `icc`
 * takes precedence: a parametric-extractable profile behaves like
 * `space`; a LUT-only profile is baked into a 3D LUT at creation and
 * sampled by the canvas image pipeline. Both are read only during
 * flux_image_create. Tagging changes how texels are decoded into the
 * working space — it never re-encodes the stored pixels. */
typedef struct optics_gfx_image_color_space_desc {
    optics_gfx_struct_type type; /* FLUX_TYPE_IMAGE_COLOR_SPACE_DESC */
    const void *next;
    const optics_gfx_color_space *space; /* nullable */
    const optics_gfx_icc_profile *icc;   /* nullable; precedence over space */
} optics_gfx_image_color_space_desc;

#define OPTICS_GFX_IMAGE_COLOR_SPACE_DESC_INIT {.type = OPTICS_GFX_TYPE_IMAGE_COLOR_SPACE_DESC}

OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result optics_gfx_image_create(
    optics_gfx_device *d, const optics_gfx_image_desc *desc, optics_gfx_image **out);
OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_image *
optics_gfx_image_retain(optics_gfx_image *image);
OPTICS_GFX_API void optics_gfx_image_release(optics_gfx_image *image);
/* Publish an immutable snapshot handle. The snapshot and the original share
 * the current pixels until the original is updated; an update then copies the
 * old version so the snapshot keeps rendering the content it captured. The
 * snapshot itself rejects update_region. A borrowed dma-buf import cannot be
 * snapshotted; bind it through a dynamic DisplayList input slot instead.
 *
 * Threading: snapshot and update_region* are serialized per device. A caller
 * must own a reference for the duration of either call; releasing the last
 * reference while another thread is using the handle is undefined. Releasing
 * one of several references concurrently is safe. */
OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result
optics_gfx_image_snapshot(optics_gfx_image *image, optics_gfx_image **out);
/* Content bytes described by the image extent and pixel format. This is the
 * pinned-content accounting unit for display-list budgets, not the driver's
 * allocation size. */
OPTICS_GFX_API size_t optics_gfx_image_size_bytes(const optics_gfx_image *image);
OPTICS_GFX_API uint32_t optics_gfx_image_width(const optics_gfx_image *image);
OPTICS_GFX_API uint32_t optics_gfx_image_height(const optics_gfx_image *image);
OPTICS_GFX_API optics_gfx_format optics_gfx_image_format(const optics_gfx_image *image);
/* Owning device, borrowed (not retained). Compute-writable images hold a
 * weak device reference, so the returned pointer is only as long-lived as
 * the caller's own device reference. */
OPTICS_GFX_API optics_gfx_device *optics_gfx_image_device(const optics_gfx_image *image);

/* Render-target images (FLUX_IMAGE_USAGE_RENDER_TARGET) bracket their pass: */
OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result
optics_gfx_frame_prepare_image_target(optics_gfx_frame *frame, optics_gfx_image *target);
OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result
optics_gfx_frame_finish_image_target(optics_gfx_frame *frame, optics_gfx_image *target);

/* Capability query — discover support instead of failing on it. Returns false
 * (never an error) for unknown formats, null devices, or a usage this device
 * cannot provide for `format`. */
OPTICS_GFX_API bool optics_gfx_device_supports_image_usage(const optics_gfx_device *d,
                                                           optics_gfx_format format,
                                                           optics_gfx_image_usage usage);

/* Upload tightly packed pixels into an in-bounds sub-region. The image view
 * and bindless handle remain valid across the update. Requires the image to
 * be in its steady sampled state (SHADER_READ_ONLY_OPTIMAL — true after
 * flux_image_create and after flux_frame_finish_image_target); anything
 * else returns FLUX_ERROR_INVALID_STATE. */
typedef struct optics_gfx_image_update_region_opts {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    const void *data;
    size_t row_bytes;
    size_t bytes;
    bool premultiply;
} optics_gfx_image_update_region_opts;

OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result optics_gfx_image_update_region(
    optics_gfx_image *image, const optics_gfx_image_update_region_opts *opts);

/* Emit a printf-style diagnostic through a device's logger (if any).
 * No-op when the device has no logger wired. Modules that treat
 * `flux_device` as opaque (and consumers) use this instead of
 * dereferencing `flux_device_desc.log` directly; it forwards to the
 * same `flux_log_fn` the desc supplied. `category` is a short tag the
 * logger can filter on (e.g. "glyph", "vulkan-validation"). */
OPTICS_GFX_API void optics_gfx_device_log(optics_gfx_device *d, optics_gfx_log_level level,
                                          const char *category, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 4, 5)))
#endif
    ;

/* Pipeline-cache persistence hooks (caller-supplied, optional).
 *
 * Modelled on Skia's PersistentCache: the library owns the in-memory
 * VkPipelineCache; the consumer owns the cross-session storage
 * strategy (a file, a content store, nothing). flux never touches the
 * filesystem — without these hooks the cache lives only for the
 * device lifetime and is discarded at release.
 *
 * Wire both into flux_device_desc to opt in:
 *
 *   .pipeline_cache_load     = my_load,
 *   .pipeline_cache_save     = my_save,
 *   .pipeline_cache_userdata = &my_state,
 *
 *   load  — return a malloc()-allocated seed blob (the library frees
 *           it with free() after feeding it to VkPipelineCache). Set
 *           *out_size = 0 / return NULL when there is no prior cache;
 *           the cache then starts cold. May be NULL even if save is
 *           set (read-only cache).
 *   save  — receives the blob produced by vkGetPipelineCacheData. The
 *           library owns `data` for the call's duration; copy it if
 *           you need it beyond the return. Called once, at
 *           flux_device_release, after all GPU work has drained. May
 *           be NULL (write-only / fresh cache each run).
 *
 * Both callbacks receive the same `userdata` pointer set on the desc.
 * They are invoked from the thread calling flux_device_create /
 * flux_device_release. */
typedef void *(*optics_gfx_pipeline_cache_load_fn)(void *userdata, size_t *out_size);
typedef void (*optics_gfx_pipeline_cache_save_fn)(void *userdata, const void *data, size_t size);

/* ================================================================== */
/*  Device                                                            */
/* ================================================================== */

typedef enum optics_gfx_validation_mode : uint32_t {
    OPTICS_GFX_VALIDATION_AUTO = 0, /* on iff debug build */
    OPTICS_GFX_VALIDATION_OFF = 1,
    OPTICS_GFX_VALIDATION_ON = 2,
} optics_gfx_validation_mode;

/* Optional flux_device_desc extension that makes physical-device selection
 * strict. The target is a Linux DRM primary or render node identified by its
 * character-device major/minor pair. A Vulkan physical device matches when
 * VK_EXT_physical_device_drm reports the target as either its primary or
 * render node. If the extension is unavailable or no matching device also
 * satisfies the base descriptor, flux_device_create returns
 * FLUX_ERROR_UNSUPPORTED; it never falls back to a different GPU. */
typedef struct optics_gfx_device_drm_node_desc {
    optics_gfx_struct_type type; /* FLUX_TYPE_DEVICE_DRM_NODE_DESC */
    const void *next;
    uint32_t drm_major;
    uint32_t drm_minor;
} optics_gfx_device_drm_node_desc;

#define OPTICS_GFX_DEVICE_DRM_NODE_DESC_INIT {.type = OPTICS_GFX_TYPE_DEVICE_DRM_NODE_DESC}

/* Semantic device capabilities. These keep callers independent of the Vulkan
 * extension bundle used by the current backend. A required capability rejects
 * physical devices that cannot provide it. An optional capability is enabled
 * when the normally selected physical device supports its complete bundle. */
typedef uint64_t optics_gfx_device_feature_flags;

#define OPTICS_GFX_DEVICE_FEATURE_DMABUF 0x0000000000000001ULL
#define OPTICS_GFX_DEVICE_FEATURE_DMABUF_SYNC_FILE 0x0000000000000002ULL

typedef struct optics_gfx_device_features_desc {
    optics_gfx_struct_type type; /* FLUX_TYPE_DEVICE_FEATURES_DESC */
    const void *next;
    optics_gfx_device_feature_flags required;
    optics_gfx_device_feature_flags optional;
} optics_gfx_device_features_desc;

#define OPTICS_GFX_DEVICE_FEATURES_DESC_INIT {.type = OPTICS_GFX_TYPE_DEVICE_FEATURES_DESC}

/* DRM character-device identities reported by the selected Vulkan physical
 * device. `has_primary` / `has_render` gate the corresponding value. */
typedef struct optics_gfx_drm_node_identity {
    uint32_t major;
    uint32_t minor;
} optics_gfx_drm_node_identity;

typedef struct optics_gfx_drm_device_identity {
    bool has_primary;
    optics_gfx_drm_node_identity primary;
    bool has_render;
    optics_gfx_drm_node_identity render;
} optics_gfx_drm_device_identity;

typedef struct optics_gfx_device_desc {
    optics_gfx_struct_type type; /* FLUX_TYPE_DEVICE_DESC */
    const void *next;

    optics_gfx_allocator allocator; /* zero-init = libc malloc */
    optics_gfx_log_fn log;          /* NULL = silent */
    void *log_user;

    /* Optional pipeline-cache persistence (see the typedefs above).
     * NULL throughout = pure in-memory cache, discarded at release. */
    optics_gfx_pipeline_cache_load_fn pipeline_cache_load;
    optics_gfx_pipeline_cache_save_fn pipeline_cache_save;
    void *pipeline_cache_userdata;

    /* Vulkan bootstrap */
    const char *const *required_instance_extensions;
    uint32_t required_instance_extension_count;
    const char *const *required_device_extensions;
    uint32_t required_device_extension_count;

    optics_gfx_validation_mode validation;

    /* Frames in flight (0 = library default, currently 2) */
    uint32_t frames_in_flight;

    /* Headless if true — no surface, no swapchain, no presentation
     * queue family requirement. */
    bool headless;
} optics_gfx_device_desc;

#define OPTICS_GFX_DEVICE_DESC_INIT {.type = OPTICS_GFX_TYPE_DEVICE_DESC}

OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result
optics_gfx_device_create(const optics_gfx_device_desc *desc, optics_gfx_device **out_device);

/* Semantic capabilities enabled on the logical device. This includes
 * capabilities requested through flux_device_features_desc and capabilities
 * implied by a complete legacy required-device-extension bundle. */
OPTICS_GFX_API optics_gfx_device_feature_flags
optics_gfx_device_enabled_features(const optics_gfx_device *d);

/* Device limits relevant to flux callers — the query the API previously
 * lacked (feature discovery was "try it and watch it fail"). Values come
 * from the Vulkan physical device; every field is zeroed when `d` is
 * NULL. Out-param form so the struct can grow: zero-init, fill what you
 * know, pass a pointer; unknown-to-the-caller trailing fields are simply
 * not written by older headers' expectations (the library writes the
 * whole struct as it knows it, so link against matching headers or rely
 * on the documented pre-1.0 rebuild rule). */
typedef struct optics_gfx_device_limits {
    uint32_t struct_size;           /* sizeof(flux_device_limits)             */
    uint32_t max_image_dimension1d; /* max flux_image_create width            */
    uint32_t max_image_dimension2d; /* max width/height of 2D images/surfaces */
    uint32_t max_image_dimension3d; /* max extent of 3D images (0 = none)     */
    uint32_t max_color_attachments; /* per-pass color attachment count        */
    uint32_t max_framebuffer_width;
    uint32_t max_framebuffer_height;
    uint32_t max_frames_in_flight; /* library cap actually applied (see
                                    * FLUX_MAX_FRAMES_IN_FLIGHT)             */
    uint64_t min_uniform_buffer_offset_alignment;
    uint64_t min_storage_buffer_offset_alignment;
    uint64_t buffer_image_granularity;
    float timestamp_period_ns;     /* ns per timestamp tick; 0 if unsupported */
    uint32_t timestamp_valid_bits; /* 0 = timestamps unavailable             */
} optics_gfx_device_limits;

#define OPTICS_GFX_DEVICE_LIMITS_INIT {.struct_size = sizeof(optics_gfx_device_limits)}

/* Fill `out` with the device's limits. Returns FLUX_ERROR_INVALID_ARGUMENT
 * if `out` is NULL; never fails otherwise. */
OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result
optics_gfx_device_get_limits(const optics_gfx_device *d, optics_gfx_device_limits *out);

/* Returns the DRM primary/render identities of the selected Vulkan physical
 * device. The output is zeroed and false is returned when
 * VK_EXT_physical_device_drm is unavailable or reports neither node. */
OPTICS_GFX_API bool
optics_gfx_device_get_drm_identity(const optics_gfx_device *d,
                                   optics_gfx_drm_device_identity *out_identity);

OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_device *
optics_gfx_device_retain(optics_gfx_device *d);
OPTICS_GFX_API void optics_gfx_device_release(optics_gfx_device *d);

OPTICS_GFX_API void optics_gfx_device_wait_idle(const optics_gfx_device *d);

/* ================================================================== */
/*  Device-owned host allocation (ADR-0135)                           */
/* ================================================================== */

/* Peer libraries must allocate through here rather than libc, so the
 * consumer's flux_allocator is honoured and the memory is attributed in the
 * observation ledger.
 *
 * The ownership pair is required, not optional: before it existed, routing a
 * library through this funnel re-attributed its leaks to "flux/core", which is
 * the misattribution ADR-0134 removed. `owner` and `kind` must be literals
 * (they are interned by content, not copied).
 *
 * Break-clean note: this replaced the positional
 * `flux_device_alloc(d, bytes)` intentionally. The old form could not express
 * ownership, and a `_ex` tier is forbidden by docs/dev/standards/api-design.md. */
typedef struct optics_gfx_device_alloc_opts {
    optics_gfx_struct_type type; /* FLUX_TYPE_DEVICE_ALLOC_OPTS */
    const void *next;
    size_t bytes;
    const char *owner;      /* subsystem: "prism/material", "glyph/core", … */
    const char *kind;       /* resource: "filter_mica", "atlas", "store", … */
    optics_gfx_scope scope; /* DEVICE for device-bound, CONTEXT for context-bound */
} optics_gfx_device_alloc_opts;

#define OPTICS_GFX_DEVICE_ALLOC_OPTS_INIT {.type = OPTICS_GFX_TYPE_DEVICE_ALLOC_OPTS}

/* Returned memory is zeroed, matching the previous contract. On success `out`
 * receives the pointer; on failure it is set to nullptr and the result says
 * why. */
OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result
optics_gfx_device_alloc(optics_gfx_device *d, const optics_gfx_device_alloc_opts *opts, void **out);

/* Release. The (owner, kind) pair must match the allocation: the ledger needs
 * it to close the row, and a mismatch is reported as an unmatched release.
 * A host may legitimately free from a different call site than it allocated in,
 * which is why the pair is repeated rather than stored in a hidden header. */
typedef struct optics_gfx_device_free_opts {
    optics_gfx_struct_type type; /* FLUX_TYPE_DEVICE_FREE_OPTS */
    const void *next;
    void *ptr; /* nullptr is a no-op */
    const char *owner;
    const char *kind;
    optics_gfx_scope scope;
} optics_gfx_device_free_opts;

#define OPTICS_GFX_DEVICE_FREE_OPTS_INIT {.type = OPTICS_GFX_TYPE_DEVICE_FREE_OPTS}

OPTICS_GFX_API void optics_gfx_device_free(optics_gfx_device *d,
                                           const optics_gfx_device_free_opts *opts);

#define OPTICS_GFX_MAX_MEMORY_HEAPS 16

typedef struct optics_gfx_memory_budget {
    uint64_t heap_bytes_total[OPTICS_GFX_MAX_MEMORY_HEAPS];
    uint64_t heap_bytes_used[OPTICS_GFX_MAX_MEMORY_HEAPS];
    uint64_t heap_budget[OPTICS_GFX_MAX_MEMORY_HEAPS];
    uint32_t heap_count;
    bool has_budget_extension;
} optics_gfx_memory_budget;

OPTICS_GFX_API void optics_gfx_device_memory_budget(const optics_gfx_device *d,
                                                    optics_gfx_memory_budget *out);

/* Point-in-time snapshot of the GPU memory allocator. bytes_in_use is
 * what live resources (pooled sub-allocations, dedicated and external
 * dma-buf memory) actually occupy; bytes_reserved is what the allocator
 * holds from the driver in total, including empty pool blocks kept for
 * reuse. All counters return to zero once every resource is released
 * and the retire queue has drained (see flux_device_wait_idle). */
typedef struct optics_gfx_memory_stats {
    uint64_t bytes_in_use;
    uint64_t bytes_reserved;
    uint64_t lost_ranges_bytes; /* freed bytes unrecoverable until block reclaim */
    uint32_t live_allocations;
    uint32_t live_blocks;
} optics_gfx_memory_stats;

/* Sample live allocator counters. Deferred upload work (staging buffers
 * still in flight) is settled before sampling so the numbers do not
 * depend on GPU timing; this makes the call wait on outstanding upload
 * fences, so keep it off hot paths. */
OPTICS_GFX_API void optics_gfx_device_memory_stats(optics_gfx_device *d,
                                                   optics_gfx_memory_stats *out);

/* Batched uploads. Between begin and flush, upload work from
 * flux_image_create, flux_image_update_region, flux_mesh_create and
 * flux_buffer_create (GPU_LOCAL with initial_data) accumulates into one
 * queue submission instead of one submission per resource — this is
 * the fast path for bulk asset loading.
 *
 * Upload submissions are deferred: flush (and the one-shot helpers
 * outside a batch) submit the copy work and return without waiting for
 * the GPU. A resource is safe to sample as soon as its creating call
 * returns, because every later batch on the same queue is ordered after
 * the copies; flux_surface_begin_frame flushes any open batch before
 * recording, so an unflushed batch can never be sampled by a frame.
 * Non-frame consumers (compute dispatch, readback) must flush
 * explicitly. begin/flush are device-global; the upload calls
 * themselves may run on any thread. Nested begin is reference-counted:
 * it returns FLUX_OK and bumps the batch depth so every scope's flush
 * wait covers it; only the outermost flush submits (inner uploads are
 * already recorded in the same batch). Flushing with no batch open is
 * a harmless no-op. */
OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result
optics_gfx_uploads_begin(optics_gfx_device *d);
OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result
optics_gfx_uploads_flush(optics_gfx_device *d);

/* ================================================================== */
/*  Buffer                                                            */
/* ================================================================== */

/* Bitmask of intended uses; OR these together in flux_buffer_desc.usage.
 * The library maps them to VkBufferUsageFlags internally and adds
 * TRANSFER_DST automatically when initial_data is provided. */
typedef enum optics_gfx_buffer_usage : uint32_t {
    OPTICS_GFX_BUFFER_USAGE_VERTEX = 1u << 0,
    OPTICS_GFX_BUFFER_USAGE_INDEX = 1u << 1,
    OPTICS_GFX_BUFFER_USAGE_UNIFORM = 1u << 2,
    OPTICS_GFX_BUFFER_USAGE_STORAGE = 1u << 3,
    OPTICS_GFX_BUFFER_USAGE_TRANSFER_SRC = 1u << 4,
    OPTICS_GFX_BUFFER_USAGE_TRANSFER_DST = 1u << 5,
} optics_gfx_buffer_usage;

typedef enum optics_gfx_buffer_location : uint32_t {
    /* DEVICE_LOCAL memory: fastest GPU access; not host-mappable.
     * Use initial_data or a TRANSFER_SRC buffer to populate. */
    OPTICS_GFX_BUFFER_GPU_LOCAL = 0,
    /* HOST_VISIBLE | HOST_COHERENT: persistent CPU map. Slower GPU
     * access but writable from the CPU every frame (uniforms, dynamic
     * vertex data). */
    OPTICS_GFX_BUFFER_HOST_VISIBLE = 1,
} optics_gfx_buffer_location;

typedef struct optics_gfx_buffer_desc {
    optics_gfx_struct_type type; /* FLUX_TYPE_BUFFER_DESC */
    const void *next;
    size_t size;
    uint32_t usage; /* OR of flux_buffer_usage bits */
    optics_gfx_buffer_location location;
    /* If true, the buffer is created with SHADER_DEVICE_ADDRESS and
     * flux_buffer_device_address() returns the 64-bit address. */
    bool device_address;
    /* Optional: upload `size` bytes at create time. For GPU_LOCAL
     * buffers this routes through the staging path; for HOST_VISIBLE
     * it's a direct memcpy into the mapped pointer. */
    const void *initial_data;
} optics_gfx_buffer_desc;

#define OPTICS_GFX_BUFFER_DESC_INIT {.type = OPTICS_GFX_TYPE_BUFFER_DESC}

OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result optics_gfx_buffer_create(
    optics_gfx_device *d, const optics_gfx_buffer_desc *desc, optics_gfx_buffer **out);

OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_buffer *
optics_gfx_buffer_retain(optics_gfx_buffer *b);
/* Destruction is deferred: a released buffer may still be referenced by
 * in-flight frames, so the VkBuffer and its memory are destroyed by the
 * device retire queue only after the GPU provably passed every batch
 * that could reference them. */
OPTICS_GFX_API void optics_gfx_buffer_release(optics_gfx_buffer *b);

/* Persistent mapped pointer; NULL on a GPU_LOCAL buffer. */
OPTICS_GFX_API void *optics_gfx_buffer_mapped(const optics_gfx_buffer *b);
OPTICS_GFX_API size_t optics_gfx_buffer_size(const optics_gfx_buffer *b);

/* ================================================================== */
/*  Render target                                                     */
/*                                                                    */
/*  A refcounted depth or colour image + view that a caller owns and  */
/*  hands to a pass via flux_pass_desc (peer-defined attachments,     */
/*  ADR-0001). This is how consumers get a depth attachment without    */
/*  writing raw Vulkan image/memory/view plumbing: flux owns the      */
/*  image, the backing GPU allocator memory, and the view, mirroring  */
/*  the refcount lifecycle of flux_buffer / flux_mesh.                */
/* ================================================================== */

typedef enum optics_gfx_target_usage : uint32_t {
    OPTICS_GFX_TARGET_DEPTH = 1u << 0, /* depth/stencil attachment image  */
    OPTICS_GFX_TARGET_COLOR = 1u << 1, /* colour attachment image         */
} optics_gfx_target_usage;

typedef struct optics_gfx_target_desc {
    optics_gfx_struct_type type; /* FLUX_TYPE_TARGET_DESC */
    const void *next;
    uint32_t usage;           /* OR of flux_target_usage */
    optics_gfx_format format; /* e.g. FLUX_FORMAT_D32_SFLOAT */
    uint32_t width;
    uint32_t height;
} optics_gfx_target_desc;

#define OPTICS_GFX_TARGET_DESC_INIT {.type = OPTICS_GFX_TYPE_TARGET_DESC}

typedef struct optics_gfx_cpu_target_desc {
    optics_gfx_struct_type type; /* FLUX_TYPE_CPU_TARGET_DESC */
    const void *next;
    uint32_t width;
    uint32_t height;
    optics_gfx_format format; /* RGBA8_UNORM only; UNDEFINED selects RGBA8_UNORM. */
    void *user_buffer;        /* optional external host memory; if NULL, target allocates */
    size_t buffer_bytes;      /* required capacity when user_buffer is supplied */
    size_t stride_bytes;      /* optional row pitch (0 = tightly packed) */
} optics_gfx_cpu_target_desc;

#define OPTICS_GFX_CPU_TARGET_DESC_INIT {.type = OPTICS_GFX_TYPE_CPU_TARGET_DESC}

OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result optics_gfx_target_create(
    optics_gfx_device *d, const optics_gfx_target_desc *desc, optics_gfx_target **out);
OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result
optics_gfx_target_create_cpu(const optics_gfx_cpu_target_desc *desc, optics_gfx_target **out);
OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result optics_gfx_target_create_from_image(
    optics_gfx_device *d, optics_gfx_image *image, optics_gfx_target **out);

OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_target *
optics_gfx_target_retain(optics_gfx_target *t);
/* Destruction is deferred: a released target may still be an attachment
 * of in-flight frames, so the image, view, and memory are destroyed by
 * the device retire queue only after the GPU provably passed every batch
 * that could reference them. */
OPTICS_GFX_API void optics_gfx_target_release(optics_gfx_target *t);

OPTICS_GFX_API uint32_t optics_gfx_target_width(const optics_gfx_target *t);
OPTICS_GFX_API uint32_t optics_gfx_target_height(const optics_gfx_target *t);

/* Return host memory buffer for CPU targets. Returns NULL if `t` is a GPU target. */
OPTICS_GFX_API const uint8_t *optics_gfx_target_cpu_pixels(const optics_gfx_target *t,
                                                           uint32_t *out_width,
                                                           uint32_t *out_height,
                                                           uint32_t *out_stride);

/* Record the layout transition that makes `target` usable as the attachment
 * selected by its usage. The target's previous contents are discarded, so
 * the following pass must clear or otherwise fully overwrite it. Call once
 * per frame before flux_frame_begin_pass. A target reused across frames must
 * be dedicated to that frame-in-flight slot (or otherwise externally
 * synchronized). */
OPTICS_GFX_API void optics_gfx_frame_prepare_target(optics_gfx_frame *f,
                                                    const optics_gfx_target *target);

/* ================================================================== */
/*  Surface                                                           */
/* ================================================================== */

typedef struct optics_gfx_surface_desc {
    optics_gfx_struct_type type; /* FLUX_TYPE_SURFACE_DESC */
    const void *next;
    void *vk_surface_khr; /* VkSurfaceKHR; caller-created via GLFW/SDL/raw.
                           * NULL selects an OFFSCREEN surface (ADR-0013):
                           * no swapchain or window; flux owns RGBA8 color
                           * images at width x height (both required
                           * non-zero). The frame loop is unchanged —
                           * flux_frame_present completes the frame without
                           * presenting — and flux_surface_read_pixels
                           * reads the result back. */
    uint32_t width;
    uint32_t height;
    bool hdr_preferred; /* request scRGB/HDR10 if available; ignored offscreen */
    bool vsync;         /* true = FIFO, false = MAILBOX/IMMEDIATE; ignored offscreen */
} optics_gfx_surface_desc;

#define OPTICS_GFX_SURFACE_DESC_INIT {.type = OPTICS_GFX_TYPE_SURFACE_DESC}

/* Optional flux_surface_desc extension for an exportable offscreen surface.
 * `modifiers` is the consumer-supported DRM_FORMAT_MOD_* set. Flux intersects
 * it with the Vulkan device's renderable/exportable single-plane modifiers;
 * surface creation fails with FLUX_ERROR_UNSUPPORTED when the intersection is
 * empty. The array is read only during flux_surface_create. */
typedef struct optics_gfx_surface_dmabuf_desc {
    optics_gfx_struct_type type; /* FLUX_TYPE_SURFACE_DMABUF_DESC */
    const void *next;
    const uint64_t *modifiers;
    uint32_t modifier_count;
} optics_gfx_surface_dmabuf_desc;

#define OPTICS_GFX_SURFACE_DMABUF_DESC_INIT {.type = OPTICS_GFX_TYPE_SURFACE_DMABUF_DESC}

/* Optional flux_surface_desc extension for a CPU-readable offscreen surface.
 * On a dma-buf-capable device an offscreen surface is normally made
 * exportable, which transfers each submitted image to the foreign consumer
 * and makes flux_surface_read_pixels refuse it. Setting `require_readback`
 * keeps the surface non-exportable (OPTIMAL-tiling, slab-allocated) so
 * readback always works — the right pick for screenshot/capture targets
 * that never leave the process. Mutually exclusive with
 * flux_surface_dmabuf_desc (FLUX_ERROR_INVALID_ARGUMENT). Ignored on
 * windowed surfaces. */
typedef struct optics_gfx_surface_readback_desc {
    optics_gfx_struct_type type; /* FLUX_TYPE_SURFACE_READBACK_DESC */
    const void *next;
    bool require_readback;
} optics_gfx_surface_readback_desc;

#define OPTICS_GFX_SURFACE_READBACK_DESC_INIT {.type = OPTICS_GFX_TYPE_SURFACE_READBACK_DESC}

/* Optional flux_surface_desc extension constraining the pixel format of
 * an OFFSCREEN surface, most preferred first (the format field ADR-0013
 * anticipated). The container must suit the transfer function of the
 * space being written: sRGB/gamma encodings accept any colour format,
 * PQ/HLG require FLUX_FORMAT_RGB10A2_UNORM or FLUX_FORMAT_RGBA16_SFLOAT,
 * linear requires FLUX_FORMAT_RGBA16_SFLOAT. Flux picks the first listed
 * format that is both suitable and device-supported (including dma-buf
 * modifier support when flux_surface_dmabuf_desc requests export) and
 * fails with FLUX_ERROR_UNSUPPORTED when nothing qualifies.
 *
 * Without this extension the container is derived from the transfer:
 * BGRA8 for sRGB/gamma (the historic default), RGB10A2 then RGBA16F for
 * PQ/HLG, RGBA16F for linear. Ignored on windowed surfaces, where the
 * swapchain negotiation owns the format. The array is read only during
 * flux_surface_create; the winner is reported via flux_surface_info.format. */
typedef struct optics_gfx_surface_offscreen_format_desc {
    optics_gfx_struct_type type; /* FLUX_TYPE_SURFACE_OFFSCREEN_FORMAT_DESC */
    const void *next;
    const optics_gfx_format *formats;
    uint32_t format_count;
} optics_gfx_surface_offscreen_format_desc;

#define OPTICS_GFX_SURFACE_OFFSCREEN_FORMAT_DESC_INIT                                              \
    {.type = OPTICS_GFX_TYPE_SURFACE_OFFSCREEN_FORMAT_DESC}

/* Optional flux_surface_desc extension listing the color spaces the
 * application can present, most preferred first (ADR-0069). Flux walks
 * the list in order and picks the first space the swapchain can do;
 * the winner is reported through flux_surface_info.color_space.
 *
 * Without this extension the legacy behavior is kept: `hdr_preferred`
 * maps to the preference list [BT2020_PQ, SCRGB, SRGB], otherwise
 * [SRGB] — the exact pick order pick_format always had.
 *
 * Spaces without a Vulkan swapchain representation (e.g. SDR BT.2020,
 * Adobe RGB, custom primaries) never match on a windowed surface;
 * on an offscreen surface the first listed space is adopted verbatim,
 * with the pixel container chosen by transfer function (see
 * flux_surface_offscreen_format_desc). The array is read only during
 * flux_surface_create. */
typedef struct optics_gfx_surface_color_space_desc {
    optics_gfx_struct_type type; /* FLUX_TYPE_SURFACE_COLOR_SPACE_DESC */
    const void *next;
    const optics_gfx_color_space *spaces;
    uint32_t space_count;
} optics_gfx_surface_color_space_desc;

#define OPTICS_GFX_SURFACE_COLOR_SPACE_DESC_INIT {.type = OPTICS_GFX_TYPE_SURFACE_COLOR_SPACE_DESC}

/* Optional flux_surface_desc extension tuning HDR presentation
 * (ADR-0069). Meaningful only when the negotiated color space is HDR
 * (PQ or HLG); ignored otherwise.
 *
 * `sdr_white_nits` places SDR white (working-space 1.0) on the HDR
 * output — 0 selects the default 203 cd/m² (ITU-R BT.2408 graphics
 * white). Raise it toward the panel's SDR brightness when mixing with
 * bright SDR windows (the Windows "SDR brightness" slider semantic).
 *
 * The remaining fields carry HDR10 static metadata
 * (SMPTE ST 2086 / CTA-861.3): with `has_metadata` set and the device
 * advertising VK_EXT_hdr_metadata, flux calls vkSetHdrMetadataEXT on
 * the (re)created swapchain. Mastering chromaticities are CIE 1931 xy;
 * luminances are cd/m² (min in the ST 2086 0.0001-unit sense is
 * NOT used here — plain cd/m²). */
typedef struct optics_gfx_surface_hdr_desc {
    optics_gfx_struct_type type; /* FLUX_TYPE_SURFACE_HDR_DESC */
    const void *next;
    float sdr_white_nits; /* 0 = 203 default */
    bool has_metadata;
    struct {
        float rx, ry, gx, gy, bx, by;
        float wx, wy;
    } mastering;         /* mastering display primaries + white, CIE xy */
    float max_luminance; /* cd/m² */
    float min_luminance; /* cd/m² */
    float max_cll;       /* MaxCLL, cd/m² */
    float max_fall;      /* MaxFALL, cd/m² */
} optics_gfx_surface_hdr_desc;

#define OPTICS_GFX_SURFACE_HDR_DESC_INIT {.type = OPTICS_GFX_TYPE_SURFACE_HDR_DESC}

/* Optional flux_surface_desc extension pinning the space pixels are
 * *written in*, decoupled from the swapchain's negotiated space
 * (ADR-0069). This is the legacy-platform display-profile path: the
 * host supplies the display's actual color space (parametric, or an
 * ICC profile extractable to one), flux converts working space ->
 * content space in the output transform, and the swapchain stays a
 * plain sRGB/scRGB container the compositor passes through.
 *
 * Do NOT combine this with color-managed compositors (Wayland
 * color-management, Windows ACM): there the compositor performs the
 * display transform and double-conversion results. On those platforms
 * negotiate the true space via flux_surface_color_space_desc instead.
 *
 * Offscreen surfaces: equivalent to requesting the space through
 * flux_surface_color_space_desc (the container follows the transfer;
 * see flux_surface_offscreen_format_desc). */
typedef struct optics_gfx_surface_output_color_desc {
    optics_gfx_struct_type type; /* FLUX_TYPE_SURFACE_OUTPUT_COLOR_DESC */
    const void *next;
    optics_gfx_color_space content_space; /* the display's actual space */
    const optics_gfx_icc_profile *icc;    /* optional; must be parametric-extractable */
} optics_gfx_surface_output_color_desc;

#define OPTICS_GFX_SURFACE_OUTPUT_COLOR_DESC_INIT                                                  \
    {.type = OPTICS_GFX_TYPE_SURFACE_OUTPUT_COLOR_DESC}

typedef struct optics_gfx_surface_info {
    uint32_t width;
    uint32_t height;
    uint32_t image_count;
    bool hdr; /* surface is actually HDR (PQ, HLG, or extended-linear) */
    /* The color space the surface actually presents in (the negotiated
     * swapchain space). Content rendered through flux is transformed
     * into `content_space` at output (ADR-0069). */
    optics_gfx_color_space color_space;
    /* What pixels are written in: equals color_space unless a
     * flux_surface_output_color_desc pinned the display's actual space
     * (the legacy-platform pre-conversion path). */
    optics_gfx_color_space content_space;
    /* Pixel format of the surface's images: the negotiated swapchain
     * format on a windowed surface, the chosen container on an offscreen
     * one. Compositors mapping the dma-buf export to a DRM fourcc key
     * off this. */
    optics_gfx_format format;
} optics_gfx_surface_info;

/* Pixel-aligned surface region used by asynchronous frame readback. Coordinates
 * are in the rendered surface's top-left-origin physical pixel space. Width and
 * height must be non-zero and the complete region must lie inside the surface. */
typedef struct optics_gfx_readback_region {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} optics_gfx_readback_region;

OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result
optics_gfx_surface_create(optics_gfx_device *device, const optics_gfx_surface_desc *desc,
                          optics_gfx_surface **out_surface);

OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_surface *
optics_gfx_surface_retain(optics_gfx_surface *s);
OPTICS_GFX_API void optics_gfx_surface_release(optics_gfx_surface *s);

/* Recreate the swapchain at the new extent. Waits for this surface's
 * in-flight frames to retire (per-slot fences for offscreen surfaces,
 * graphics-queue idle for windowed ones, where the presentation engine
 * can outlive a frame fence) — a queue/slot-scoped quiesce, not a
 * device-wide stall; the transfer queue and unrelated work keep running
 * (ADR-0021). Drops and rebuilds swapchain images and per-image views;
 * in-flight flux_frame handles obtained from this surface become invalid
 * and the next flux_surface_begin_frame returns the first frame of the
 * new chain. Safe to call from the window resize callback. Returns
 * FLUX_ERROR_INVALID_ARGUMENT if w or h is 0. */
OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result
optics_gfx_surface_resize(optics_gfx_surface *s, uint32_t w, uint32_t h);
OPTICS_GFX_API void optics_gfx_surface_get_info(const optics_gfx_surface *s,
                                                optics_gfx_surface_info *out);

/* Read back a captured frame as tightly packed RGBA8, row-major, top-left
 * origin (`captured_width * captured_height * 4` bytes; `bytes` must be at
 * least that). A full-frame request uses the surface extent; a region request
 * uses that region's extent.
 * Windowed and exportable surfaces require flux_frame_request_readback on the
 * submitted frame. Plain offscreen surfaces retain their legacy behaviour of
 * reading the most recently submitted image. Waits for the relevant GPU work
 * to complete first. Returns FLUX_ERROR_INVALID_STATE when no readable frame
 * is available. */
OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result
optics_gfx_surface_read_pixels(optics_gfx_surface *s, void *dst, size_t bytes);

/* Allocate persistent readback staging ahead of the first capture. This is
 * optional (flux_frame_request_readback allocates it lazily) but lets
 * latency-sensitive callers keep allocation out of the trigger frame. Resize
 * retires this staging, so callers may prepare again afterward. */
OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result
optics_gfx_surface_prepare_readback(optics_gfx_surface *s);

/* Preallocate persistent staging for a future region readback. Only the
 * requested extent affects allocation; x/y are supplied on the frame request.
 * Existing sufficiently large staging is reused. The full-frame prepare API
 * above is equivalent to passing the surface extent here. */
OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result
optics_gfx_surface_prepare_readback_region(optics_gfx_surface *s, uint32_t width, uint32_t height);

/* Non-blocking readiness query for a frame copied to persistent readback
 * staging. `out_ready` becomes true once that frame's image-to-buffer copy has
 * completed. Returns FLUX_ERROR_INVALID_STATE before the first captured frame
 * and FLUX_ERROR_UNSUPPORTED when the surface has no persistent readback
 * staging (a plain offscreen surface before any on-demand request). */
OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result
optics_gfx_surface_read_pixels_ready(optics_gfx_surface *s, bool *out_ready);

/* Detach a completed on-demand snapshot from its surface without copying its
 * pixels. The returned immutable handle owns the mapped staging allocation,
 * can outlive or move to a different thread from the surface, and is consumed
 * with flux_readback_read_pixels before flux_readback_release. Query its exact
 * source offset/extent with flux_readback_get_region. This is
 * unsupported for require_readback surfaces, whose staging is continuously
 * surface-owned. The caller should first observe `out_ready == true`. */
OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result
optics_gfx_surface_take_readback(optics_gfx_surface *s, optics_gfx_readback **out_readback);
OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result
optics_gfx_readback_read_pixels(const optics_gfx_readback *readback, void *dst, size_t bytes);
/* Return the exact surface region captured by an immutable readback. `out_region`
 * is zeroed when `readback` is NULL. */
OPTICS_GFX_API void optics_gfx_readback_get_region(const optics_gfx_readback *readback,
                                                   optics_gfx_readback_region *out_region);
OPTICS_GFX_API void optics_gfx_readback_release(optics_gfx_readback *readback);

/* Offscreen surfaces only (and only when the device had the dma-buf
 * extensions enabled at creation): export the most recently submitted
 * frame's image memory as a Linux dma-buf file descriptor. The caller
 * owns and must close() the returned fd. Zero-copy: no GPU->CPU pixel
 * transfer; the fd is a handle to the same GPU memory the compositor
 * composites directly. Waits for the frame's GPU work to complete first.
 *
 * flux_surface_exportable() reports whether the surface was created
 * exportable (device had the needed extensions + a suitable modifier was
 * found). flux_surface_dmabuf_modifier() / _stride() return the DRM
 * modifier and row stride the host pairs with the fd when building a
 * zwp_linux_buffer_params_v1.
 *
 * Export releases the selected image from the graphics queue family to
 * VK_QUEUE_FAMILY_FOREIGN_EXT. Before that frame slot is reused, the host must
 * wait until the external consumer has released the buffer (for Wayland,
 * wl_buffer.release or a signalled explicit-sync release fence). Calling
 * begin_frame early is a caller error the library cannot observe. Likewise,
 * resize/release requires every exported buffer to be back from its consumer.
 * The v1 export contract is one plane, offset 0, BGRA8, with the returned
 * modifier and stride; surfaces that cannot meet it are not exportable.
 *
 * Returns FLUX_ERROR_UNSUPPORTED on a windowed or non-exportable surface,
 * FLUX_ERROR_INVALID_STATE before the first submitted frame. */
/* An exported offscreen image lease. The surface's frame ring will not reuse
 * the leased image slot until `flux_surface_dmabuf_lease_release` is called.
 * A lease retains its surface; callers may release it from their page-flip
 * completion path. Release is idempotent only in the sense that the pointer
 * becomes invalid: call it exactly once. */
typedef struct optics_gfx_surface_dmabuf_lease optics_gfx_surface_dmabuf_lease;

OPTICS_GFX_API bool optics_gfx_surface_exportable(const optics_gfx_surface *s);
OPTICS_GFX_API uint64_t optics_gfx_surface_dmabuf_modifier(const optics_gfx_surface *s);
OPTICS_GFX_API uint32_t optics_gfx_surface_dmabuf_stride(const optics_gfx_surface *s);

/* Export the most recently submitted frame. The returned fd is owned by the
 * caller and must be closed. The returned lease owns the image lifetime:
 * until it is released, flux_surface_begin_frame for that slot returns
 * FLUX_ERROR_BUSY. One lease may be held per submitted image. */
OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result optics_gfx_surface_export_dmabuf(
    optics_gfx_surface *s, int *out_fd, optics_gfx_surface_dmabuf_lease **out_lease);

/* Explicit-sync export for a submitted exportable offscreen frame. Returns a
 * dma-buf fd plus a Linux sync_file fd signalled by the same Vulkan submission
 * that released image ownership to VK_QUEUE_FAMILY_FOREIGN_EXT. Unlike
 * flux_surface_export_dmabuf, this does not wait for the GPU on the CPU.
 * Both descriptors and the image lease belong to the caller on success.
 * Requires VK_KHR_external_semaphore_fd and may be called once per submitted
 * slot. */
OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result
optics_gfx_surface_export_dmabuf_explicit(optics_gfx_surface *s, int *out_fd, int *out_sync_fd,
                                          optics_gfx_surface_dmabuf_lease **out_lease);

/* Release an exported image lease. After this returns the slot may be
 * reacquired by flux_surface_begin_frame on the next call. */
OPTICS_GFX_API void optics_gfx_surface_dmabuf_lease_release(optics_gfx_surface_dmabuf_lease *lease);

/* Persistent/pooled export for a consumer that publishes one fd per ring slot
 * and synchronizes each use with a per-frame acquire fence (the compositor's
 * ScreenCast slot table). No lease is returned: the host explicitly accepts
 * that the ring may reuse the image on a later frame. The consumer must finish
 * reading the slot before the next submission that reuses it; the per-frame
 * explicit sync fence is the only ordering guarantee. Prefer the leased
 * exports for KMS/one-shot external consumers. */
OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result
optics_gfx_surface_export_dmabuf_pooled(optics_gfx_surface *s, int *out_fd);
OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result
optics_gfx_surface_export_dmabuf_pooled_explicit(optics_gfx_surface *s, int *out_fd,
                                                 int *out_sync_fd);

/* Offscreen surfaces: the frame slot index of the most recently submitted
 * frame (0..frames_in_flight-1), or UINT32_MAX before the first submit.
 * The host uses this to align a per-slot dma-buf buffer pool with the
 * image whose memory flux_surface_export_dmabuf will export. */
OPTICS_GFX_API uint32_t optics_gfx_surface_last_slot(const optics_gfx_surface *s);

/* ================================================================== */
/*  Damage                                                            */
/* ================================================================== */

/* Upper bound on the rectangles one damage set may carry. A frame that needs
 * more degrades to its full extent — never to a truncated list, because a
 * dropped region leaves stale pixels on screen while an oversized one only
 * costs work. */
enum : uint32_t {
    OPTICS_GFX_DAMAGE_CAPACITY = 64,
};

/* A set of dirty rectangles in physical surface pixels. The array is borrowed
 * and only read during the call that receives it; callers may build it in a
 * temporary container.
 *
 * For a canvas pass, an empty set means "the whole destination" (see
 * flux_canvas_pass_desc::damage). For a frame's repaint scope, use the
 * companion `out_full` flag: empty + full=false means "nothing to paint". The
 * two meanings are never inferred from the same field. */
typedef struct optics_gfx_damage {
    const optics_gfx_rect *rects;
    uint32_t count;
} optics_gfx_damage;

/* ================================================================== */
/*  Frame                                                             */
/* ================================================================== */

typedef struct optics_gfx_frame_begin_desc {
    optics_gfx_struct_type type; /* FLUX_TYPE_FRAME_BEGIN_DESC */
    const void *next;
    uint64_t timeout_ns; /* 0 = FLUX_DEFAULT_FRAME_TIMEOUT_NS (2s) unless non_blocking is true */
    bool non_blocking; /* true = return FLUX_ERROR_TIMEOUT immediately if acquire/fence not ready */
    /* What changed in the scene this frame is about to record, in physical
     * surface pixels. Flux unions this with the history the acquired image
     * missed and exposes the result through flux_frame_repaint_scope().
     *
     * `damage_full` means "the whole extent changed / could not be
     * localised". Empty damage with damage_full=false means "no new damage"
     * (a history-repair or capture-only frame), not "full". A NULL `desc`
     * keeps the conservative pre-damage contract: full repaint. */
    optics_gfx_damage damage;
    bool damage_full;
} optics_gfx_frame_begin_desc;

/* Default fence/acquire timeout for frame begin, image upload, and one-shot
 * GPU submissions. 2 seconds: long enough for any realistic GPU workload,
 * short enough that a hung GPU/driver surfaces as FLUX_ERROR_TIMEOUT rather
 * than freezing the caller thread indefinitely. */
#define OPTICS_GFX_DEFAULT_FRAME_TIMEOUT_NS (2000000000ull)

/* The initializer is conservative: a caller that does not describe its damage
 * repaints the whole extent. Override `damage_full` to false only when passing
 * an explicit `damage` set (or a genuine no-change frame). */
#define OPTICS_GFX_FRAME_BEGIN_DESC_INIT                                                           \
    {.type = OPTICS_GFX_TYPE_FRAME_BEGIN_DESC, .damage_full = true}

OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result optics_gfx_surface_begin_frame(
    optics_gfx_surface *s, const optics_gfx_frame_begin_desc *desc, optics_gfx_frame **out_frame);

/* Upper bound on frames in flight per surface. Frame-slot consumers
 * (filters keyed by flux_frame_index) size their per-slot storage with
 * this. */
#define OPTICS_GFX_MAX_FRAMES_IN_FLIGHT 3

/* Frame lifecycle state. Frames follow a strict single-use machine:
 * INVALID -> RECORDING (begin_frame) -> SUBMITTED -> PRESENTED. */
typedef enum optics_gfx_frame_state : uint32_t {
    OPTICS_GFX_FRAME_STATE_INVALID = 0,
    OPTICS_GFX_FRAME_STATE_RECORDING,
    OPTICS_GFX_FRAME_STATE_SUBMITTED,
    OPTICS_GFX_FRAME_STATE_PRESENTED,
} optics_gfx_frame_state;

OPTICS_GFX_API optics_gfx_frame_state optics_gfx_frame_get_state(const optics_gfx_frame *f);
/* True between flux_frame_begin_pass and flux_frame_end_pass. Compute
 * effects must be recorded outside a pass — a recording frame with no
 * active pass is the "frame pass boundary" effect APIs require. */
OPTICS_GFX_API bool optics_gfx_frame_has_active_pass(const optics_gfx_frame *f);
/* The device of the surface this frame was begun on, borrowed (not
 * retained); NULL for a frame that has not begun. */
OPTICS_GFX_API optics_gfx_device *optics_gfx_frame_device(const optics_gfx_frame *f);

/* Capture the exact color attachment produced by this recording frame.
 * flux_frame_submit inserts an image-to-buffer copy before the final present
 * or dma-buf ownership transition. The copied pixels remain unchanged by
 * later frames and can be polled with flux_surface_read_pixels_ready, then
 * retrieved with flux_surface_read_pixels. A later request replaces the
 * surface's previous snapshot. */
OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result
optics_gfx_frame_request_readback(optics_gfx_frame *f);

/* Capture only `region` from this frame into tightly packed staging. The copy
 * is recorded in the frame's existing graphics submission using
 * VkBufferImageCopy.imageOffset/imageExtent, so GPU traffic and CPU conversion
 * are proportional to the selected region rather than the whole surface. */
OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result optics_gfx_frame_request_readback_region(
    optics_gfx_frame *f, const optics_gfx_readback_region *region);

/* Frames follow a strict single-use state machine:
 * begin_frame -> submit -> present (or abandon). Calling a transition out of
 * order or more than once returns FLUX_ERROR_INVALID_STATE. The frame pointer
 * is a surface-owned borrow and becomes invalid when present or abandon
 * returns. On offscreen surfaces, flux_frame_abandon releases the frame-slot
 * borrow without advancing current_frame, so a rejected external KMS flip
 * reuses the same free backbuffer slot on retry instead of rotating into an
 * active scanout buffer. */
OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result optics_gfx_frame_submit(optics_gfx_frame *f);
OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result optics_gfx_frame_present(optics_gfx_frame *f);
OPTICS_GFX_API void optics_gfx_frame_abandon(optics_gfx_frame *f);

/* Per-frame transient memory — GPU-visible, mapped, recycled after
 * FLUX_FRAMES_IN_FLIGHT frames. Returns aligned pointer + offset
 * relative to the underlying buffer (which lives in <flux/vulkan.h>).
 *
 *   gpu_address  is the buffer-device-address of `cpu`. It is 0 only
 *                if the device was created without the
 *                bufferDeviceAddress feature; on conformant flux
 *                devices it is always non-zero for a valid slice. */
typedef struct optics_gfx_transient {
    void *cpu;
    uint64_t gpu_address;
    size_t size;
    size_t alignment;
} optics_gfx_transient;

/* `alignment` must be a power of two between 1 and 256 inclusive.
 * Larger alignments are rejected with FLUX_ERROR_INVALID_ARGUMENT.
 * Returns FLUX_ERROR_OUT_OF_RANGE if the per-frame ring is exhausted. */
typedef struct optics_gfx_frame_alloc_transient_opts {
    size_t bytes;
    size_t alignment;
    optics_gfx_transient *out;
} optics_gfx_frame_alloc_transient_opts;

OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result optics_gfx_frame_alloc_transient(
    optics_gfx_frame *f, const optics_gfx_frame_alloc_transient_opts *opts);

/* Materialize a deferred effect output (ADR-0147).
 *
 * An effect applied through flux_blur_filter_apply records its dispatches
 * lazily: the work exists on paper the moment the call returns, but its
 * command buffer is only built and submitted when something asks for the
 * pixels. This call is that ask, for consumers that record into the frame's
 * command buffer directly (a material filter, a compute pass) rather than
 * being a deferred pass themselves.
 *
 * Must be called at a pass boundary — no canvas pass active — because
 * compute cannot be recorded inside a render pass. Returns FLUX_OK for an
 * image that is not deferred (a plain image, or an already-materialized
 * output), so callers need no special case.
 *
 * Errors:
 *   FLUX_ERROR_INVALID_ARGUMENT — null frame or image.
 *   FLUX_ERROR_INVALID_STATE    — a canvas pass is active, or the frame is
 *                                 not recording. */
OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result
optics_gfx_frame_materialize_image(optics_gfx_frame *f, optics_gfx_image *image);

/* ================================================================== */
/*  Deferred pass plan accounting (ADR-0147)                          */
/* ================================================================== */

/* An effect applied through flux_blur_filter_apply (or another deferred
 * operator) registers a pass instead of recording its dispatches. The pass
 * is built and executed only if something requires its output; otherwise it
 * is pruned at submit and costs no GPU time.
 *
 * These counters make that difference observable, which is what lets a test
 * assert pruning rather than merely hope for it. They are per frame and
 * reset by flux_surface_begin_frame. */
typedef struct optics_gfx_frame_plan_counters {
    uint32_t registered; /* passes registered this frame */
    uint32_t executed;   /* passes whose dispatches actually ran */
    uint32_t pruned;     /* registered passes nothing required */
    uint32_t fallbacks;  /* passes that fell back to eager execution */
} optics_gfx_frame_plan_counters;

/* Read the frame's plan counters. Returns FLUX_ERROR_INVALID_STATE when the
 * frame is not recording (the counters describe a live plan, not history). */
OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result
optics_gfx_frame_plan_stats(const optics_gfx_frame *f, optics_gfx_frame_plan_counters *out);

/* Frame-in-flight slot index, in [0, frames_in_flight). Per-slot GPU
 * resources (filters, readback rings) key on this value.
 *
 * This is NOT the swapchain image the frame renders into. A swapchain may
 * carry more images than there are frames in flight, and the image index is
 * chosen by the presentation engine at acquire time. Content-preservation
 * and damage history must key on `flux_frame_image_index`: one entry per
 * image, never one per frame slot. */
OPTICS_GFX_API uint32_t optics_gfx_frame_index(const optics_gfx_frame *f);

/* Swapchain image index acquired for this frame. Stable from begin_frame
 * until present. For offscreen surfaces (no swapchain) this equals the frame
 * slot. */
OPTICS_GFX_API uint32_t optics_gfx_frame_image_index(const optics_gfx_frame *f);

/* The pixels this recording frame must repaint, in physical surface pixels.
 *
 * The answer is the union of (a) every write committed since this image last
 * became current and (b) the current frame's damage passed to
 * flux_surface_begin_frame. On a windowed surface whose persistent content
 * image holds the authoritative frame, history is repaired by an internal
 * copy and the returned scope is only the current damage; the API hides that
 * distinction from the caller.
 *
 * `*out_full = true` means repaint the whole extent (the image was never
 * initialised, history overflowed, or the current damage was full).
 * `*out_full = false` with `*out_count == 0` means nothing needs painting.
 *
 * The caller owns `out_rects`; `capacity` must be at least
 * FLUX_DAMAGE_CAPACITY to receive every case. If the scope does not fit,
 * FLUX_ERROR_OUT_OF_RANGE is returned and `*out_full` is set so the caller can
 * conservatively repaint everything. */
typedef struct optics_gfx_frame_repaint_scope_opts {
    optics_gfx_rect *out_rects;
    uint32_t capacity;
    uint32_t *out_count;
    bool *out_full;
} optics_gfx_frame_repaint_scope_opts;

OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result optics_gfx_frame_repaint_scope(
    const optics_gfx_frame *f, const optics_gfx_frame_repaint_scope_opts *opts);

/* The pixels changed since this surface's most recent successful present, in
 * physical surface pixels. This is the damage a scanout/KMS consumer should
 * receive: it includes frames that were submitted and then abandoned before
 * scanout, and it is independent of which ring image the next frame acquires.
 *
 * `*out_full = true` means there is no usable history (nothing presented yet,
 * history was invalidated, or the retained window overflowed). Empty with
 * `*out_full = false` means the display is already current. */
typedef struct optics_gfx_surface_scanout_damage_opts {
    optics_gfx_rect *out_rects;
    uint32_t capacity;
    uint32_t *out_count;
    bool *out_full;
} optics_gfx_surface_scanout_damage_opts;

OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result optics_gfx_surface_scanout_damage(
    const optics_gfx_surface *s, const optics_gfx_surface_scanout_damage_opts *opts);

/* Forget every damage epoch and image-history entry. Call after the display
 * content was lost or replaced outside the normal present path (VT switch,
 * backend takeover). The next frame repaints in full. Must not be called while
 * a frame is recording. */
OPTICS_GFX_API void optics_gfx_surface_invalidate_damage(optics_gfx_surface *s);

/* ================================================================== */
/*  GPU profiling — timestamp queries                                 */
/* ================================================================== */

OPTICS_GFX_API void optics_gfx_frame_timestamp_begin(optics_gfx_frame *f, const char *label);
OPTICS_GFX_API void optics_gfx_frame_timestamp_end(optics_gfx_frame *f);

typedef struct optics_gfx_timestamp_result {
    const char *label;
    double ms; /* GPU time in milliseconds */
} optics_gfx_timestamp_result;

/* Returns timestamps from the most recent COMPLETED frame at this
 * frame's slot. Caller-supplied buffer; out_count receives the actual
 * number written. */
typedef struct optics_gfx_frame_collect_timestamps_opts {
    optics_gfx_timestamp_result *out;
    uint32_t *inout_count;
} optics_gfx_frame_collect_timestamps_opts;

OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result optics_gfx_frame_collect_timestamps(
    optics_gfx_frame *f, const optics_gfx_frame_collect_timestamps_opts *opts);

#ifdef __cplusplus
}
#endif

#endif /* FLUX_CORE_H */
