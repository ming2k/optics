/*
 * gfx/observe.h — the observation plane (ADR-0129).
 *
 * One allocation ledger for the whole stack. Three scopes, one rule:
 * a scope declares the point at which its live count must be zero, and
 * the ledger reports exactly which (scope, kind, owner) failed to drain.
 *
 * Design contract:
 *   - No Vulkan type appears in this header.
 *   - No function here allocates, formats, or takes the device allocator
 *     lock. The counters are atomics; the name/site tables are bounded
 *     and allocated once.
 *   - Counting is unconditional in every build. Naming, pass labels and
 *     creation sites are capabilities selected through
 *     optics_gfx_observe_opts, and their symbols must be absent from a build
 *     that did not request them (INV-ARCH-LEDGER).
 *   - Reporting is observational: it never mutates resource state, so it
 *     stays usable under OOM, after device loss, and on a host that has
 *     never submitted a frame.
 */

#ifndef OPTICS_GFX_OBSERVE_H
#define OPTICS_GFX_OBSERVE_H

#include <optics/gfx/core.h>
#include <optics/gfx/observe_sites.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/*  Scopes                                                            */
/* ================================================================== */

/* optics_gfx_scope lives in <optics/gfx/core.h>: core.h owns the allocation entry points
 * that take it, and observe.h cannot be a prerequisite of core.h. */
/* How a scope's drain point is enforced. Declared per row, because a
 * frame-lifetime kind may legitimately live in the context scope (a per-frame
 * scratch buffer owned by the ui store, for instance). */
typedef enum optics_gfx_observe_lifetime : uint32_t {
    /* Must return to zero at the end of every frame. Asserted in debug
     * configurations at optics_gfx_frame_present / optics_gfx_frame_abandon. */
    OPTICS_GFX_LIFETIME_FRAME = 0,
    /* Must return to zero when the owning context is released. */
    OPTICS_GFX_LIFETIME_CONTEXT = 1,
    /* Must return to zero when the process exits; reported, never spun on. */
    OPTICS_GFX_LIFETIME_PROCESS = 2,
} optics_gfx_observe_lifetime;

/* ================================================================== */
/*  Counters                                                          */
/* ================================================================== */

/* One accounted (kind, owner) pair. `live` is the only field a test may
 * assert on; `peak` and `bytes_*` exist so that high-water regressions
 * (ADR-0072) and fragmentation are never conflated with a leak. */
typedef struct optics_gfx_observe_counters {
    uint64_t live;          /* acquire - release; must reach 0 at drain */
    uint64_t peak;          /* high-water mark of `live` */
    uint64_t acquire_total; /* monotonic; survives a balanced pair */
    uint64_t release_total; /* monotonic */
    uint64_t bytes_live;    /* 0 for kinds that carry no size */
    uint64_t bytes_peak;
} optics_gfx_observe_counters;

/* One row of a report. Strings are library-owned literals; they outlive
 * the device. `name` is NULL unless OPTICS_GFX_OBSERVE_NAMES was requested;
 * `site` is NULL unless OPTICS_GFX_OBSERVE_SITES was requested. */
typedef struct optics_gfx_observe_entry {
    optics_gfx_scope scope;
    optics_gfx_observe_lifetime lifetime;
    const char *kind;  /* closed library vocabulary: "host", "vk_memory", … */
    const char *owner; /* subsystem: "gfx/core", "ui/store", "window/wayland" */
    const char *name;  /* newest named instance of this kind; NULL if off */
    uint32_t instance; /* that instance's id; 0 when naming is off */
    const char *site;  /* fixed site vocabulary, see <optics/gfx/observe_sites.h> */
    uint64_t live;
    uint64_t bytes_live;
} optics_gfx_observe_entry;

/* Structured return for every reporting entry point (api-design.md §1.4). */
typedef struct optics_gfx_observe_response {
    optics_gfx_result result;
    /* Rows written to the caller's buffer. */
    uint32_t count;
    /* Rows the ledger could report, including those that did not fit. */
    uint32_t total;
    /* Sum of `live` across every row, whether or not it fit the buffer. */
    uint64_t live_total;
    /* Kinds the ledger had to refuse because the kind table was full.
     * Non-zero means a report is *incomplete* and must not be read as
     * "everything drained". Never silently truncated. */
    uint64_t overflowed_kinds;
} optics_gfx_observe_response;

/* ================================================================== */
/*  Capabilities (ADR-0129 decision 2)                                */
/* ================================================================== */

typedef enum optics_gfx_observe_capability : uint32_t {
    /* Counting. Always on; declared here so a caller can assert the set. */
    OPTICS_GFX_OBSERVE_COUNTS = 1u << 0,
    /* Derive VkDebugUtils object names from the ledger instead of the
     * per-call-site string literals. */
    OPTICS_GFX_OBSERVE_NAMES = 1u << 1,
    /* vkCmdBeginDebugUtilsLabelEXT / End around every recorded pass. */
    OPTICS_GFX_OBSERVE_PASS_LABELS = 1u << 2,
    /* First-acquire (file, line) ring. Allocation-free; literals only. */
    OPTICS_GFX_OBSERVE_SITES = 1u << 3,
} optics_gfx_observe_capability;

/* Chain onto optics_gfx_device_desc.next (OPTICS_GFX_TYPE_OBSERVE_OPTS). */
typedef struct optics_gfx_observe_opts {
    optics_gfx_struct_type type; /* OPTICS_GFX_TYPE_OBSERVE_OPTS */
    const void *next;
    uint32_t capabilities;  /* OR of optics_gfx_observe_capability */
    uint32_t name_capacity; /* bounded name/site rows; 0 = library default */
    optics_gfx_log_fn log;  /* NULL = no teardown report (tests read the ledger) */
    void *log_user;
} optics_gfx_observe_opts;

/* Library default for name_capacity when the caller passes 0. Sized for a
 * few hundred named objects and zero dynamic growth. */
#define OPTICS_GFX_OBSERVE_DEFAULT_NAME_CAPACITY 256u

/* ================================================================== */
/*  Accounting (hot path)                                             */
/* ================================================================== */

/* Record one acquisition. `d` may be nullptr for OPTICS_GFX_SCOPE_PROCESS, whose
 * rows are process-global by nature (a display connection or wakeup eventfd
 * exists before the device does); every other scope requires a device.
 * `kind` and `owner` must be string literals (the
 * ledger interns by pointer identity — no copying, no allocation). `bytes`
 * is 0 for kinds that carry no size. `lifetime` states the drain point.
 * Safe from any thread the device is usable on. Returns false only if the
 * kind table is full, in which case the ledger's `overflowed_kinds` is
 * incremented and a later report says so.
 *
 * `site` is the literal returned by optics_gfx_site_label(site_) or NULL. */
typedef struct optics_gfx_observe_acquire_opts {
    optics_gfx_scope scope;
    optics_gfx_observe_lifetime lifetime;
    const char *kind;
    const char *owner;
    uint64_t bytes;
    const char *site;
} optics_gfx_observe_acquire_opts;

typedef struct optics_gfx_observe_release_opts {
    optics_gfx_scope scope;
    const char *kind;
    const char *owner;
    uint64_t bytes;
} optics_gfx_observe_release_opts;

OPTICS_GFX_API bool optics_gfx_observe_acquire(optics_gfx_device *d,
                                               const optics_gfx_observe_acquire_opts *opts);

/* Record one release. Same identity rules as acquire. A release with no
 * matching *live* acquisition is counted and logged (with the kind and
 * owner) instead of being dropped: an unbalanced release is a defect in
 * its own right and must not be able to masquerade as a balanced pair. */
OPTICS_GFX_API void optics_gfx_observe_release(optics_gfx_device *d,
                                               const optics_gfx_observe_release_opts *opts);

/* Convenience wrappers. The only sanctioned way to count, so a call site
 * cannot forget the site argument.
 *
 * `site_` is a optics_gfx_site enum slot, not `__FILE__ ":" __LINE__`: a per-call-site
 * format string would land one literal per counted site in the shipped object,
 * which is exactly the "site machinery" a non-attribution build promises not to
 * carry. Use OPTICS_GFX_SITE_NONE when the kind has no fixed creation site. */
#define OPTICS_GFX_OBSERVE_ACQUIRE(d, scope_, lifetime_, kind_, owner_, bytes_, site_)             \
    optics_gfx_observe_acquire((d), &(optics_gfx_observe_acquire_opts){                            \
                                        .scope = (scope_),                                         \
                                        .lifetime = (lifetime_),                                   \
                                        .kind = (kind_),                                           \
                                        .owner = (owner_),                                         \
                                        .bytes = (bytes_),                                         \
                                        .site = optics_gfx_site_label(site_),                      \
                                    })
#define OPTICS_GFX_OBSERVE_RELEASE(d, scope_, kind_, owner_, bytes_)                               \
    optics_gfx_observe_release((d), &(optics_gfx_observe_release_opts){                            \
                                        .scope = (scope_),                                         \
                                        .kind = (kind_),                                           \
                                        .owner = (owner_),                                         \
                                        .bytes = (bytes_),                                         \
                                    })

/* ================================================================== */
/*  Reporting                                                         */
/* ================================================================== */

typedef struct optics_gfx_observe_report_opts {
    optics_gfx_struct_type type; /* OPTICS_GFX_TYPE_OBSERVE_REPORT_OPTS */
    const void *next;
    /* Caller-owned row buffer. Required. */
    optics_gfx_observe_entry *entries;
    uint32_t capacity;
    /* false (default): only rows whose live count is non-zero, i.e. the
     * rows that are leaking at this drain point. true: every inherited
     * (kind, owner) row, for diagnostics and soak trend output. */
    bool include_drained;
    /* Restrict to one scope; OPTICS_GFX_SCOPE_COUNT means "all scopes". */
    optics_gfx_scope scope;
} optics_gfx_observe_report_opts;

/* Read the ledger without side effects. Never allocates, never waits on a
 * fence, never takes the allocator lock, so it stays valid under OOM,
 * after device loss, and on a device that never submitted a frame. */
/* `d` may be nullptr when `opts->scope` is OPTICS_GFX_SCOPE_PROCESS. */
OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_observe_response
optics_gfx_observe_report(const optics_gfx_device *d, const optics_gfx_observe_report_opts *opts);

/* Snapshot one (kind, owner) row. Used by leak lanes to assert an exact
 * delta around a create/release pair instead of a tolerance constant.
 * Returns OPTICS_GFX_ERROR_OUT_OF_RANGE when the row was never interned. */
typedef struct optics_gfx_observe_read_counters_opts {
    optics_gfx_scope scope;
    const char *kind;
    const char *owner;
    optics_gfx_observe_counters *out;
} optics_gfx_observe_read_counters_opts;

OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result optics_gfx_observe_read_counters(
    const optics_gfx_device *d, const optics_gfx_observe_read_counters_opts *opts);

/* Emit the drain report for `scope` through the configured logger. Called
 * by the library at the scope's drain point; also callable by a host that
 * wants a checkpoint. Rows with a non-zero live count are logged at WARN,
 * one line each, in the form:
 *
 *   observe: leak scope=device kind=vk_memory owner=gfx/core live=2
 *            bytes=8388608 name=...
 *
 * The word "possible" never appears: a scope past its drain point either
 * drained or leaked (ADR-0129 decision 3). */
OPTICS_GFX_API void optics_gfx_observe_report_emit(const optics_gfx_device *d,
                                                   optics_gfx_scope scope);

/* Emit the process-scope report through the process-global logger (the last
 * optics_gfx_device to register one). Call from an app/host shutdown path, after
 * every context is gone. This is the only report a host that never created a
 * device can still produce — the reason fd and protocol leaks used to be
 * unattributable. */
OPTICS_GFX_API void optics_gfx_observe_report_emit_process(void);

/* Frame-lifetime enforcement (ADR-0129 decision 4). Called at the frame
 * boundary in debug configurations; returns false and logs when a
 * OPTICS_GFX_LIFETIME_FRAME row is still live. Wired into optics_gfx_frame_present /
 * optics_gfx_frame_abandon, not called directly by hosts. */
OPTICS_GFX_NODISCARD OPTICS_GFX_API bool
optics_gfx_observe_frame_drain_check(const optics_gfx_device *d);

/* Labels actually handed to the driver since device creation (ADR-0133).
 * Zero when OPTICS_GFX_OBSERVE_PASS_LABELS is off, attribution is not compiled in, or
 * VK_EXT_debug_utils is unavailable. Exists so a pass-label implementation can
 * be asserted on rather than assumed: before it, nothing in the tree could tell
 * whether a label had ever been emitted. */
OPTICS_GFX_API uint64_t optics_gfx_device_labels_emitted(const optics_gfx_device *d);

/* ================================================================== */
/*  Scope conventions — the closed kind vocabulary (ADR-0129)          */
/* ================================================================== */

/* Kind strings are a closed, library-owned vocabulary. A host may not mint
 * new kinds casually: a new kind must add a lane to the resource-leak tests
 * (ADR-0130) so its acquire/release balance is asserted, not assumed. Owner
 * strings name the subsystem that owns the release, so a report row always
 * answers "who should have released this?" without a code search.
 *
 *   OPTICS_GFX_SCOPE_DEVICE
 *     "host"        gfx/core   device-owned host allocation
 *     "vk_memory"   gfx/core   VkDeviceMemory (slab or dedicated)
 *     "vk_buffer"   gfx/core   VkBuffer
 *     "vk_image"    gfx/core   VkImage + image view
 *     "vk_sampler"  gfx/core   VkSampler
 *     "vk_shader"   gfx/core   VkShaderModule
 *     "vk_layout"   gfx/core   VkPipelineLayout / VkDescriptorSetLayout
 *     "vk_pipeline" gfx/core   VkPipeline
 *     "vk_cmd"      gfx/core   VkCommandPool / VkCommandBuffer pool
 *     "vk_fence"    gfx/core   VkFence / VkSemaphore
 *     "bindless"    gfx/core   one bindless heap slot
 *
 *   OPTICS_GFX_SCOPE_CONTEXT
 *     "frame"       ui/core   one UI frame (OPTICS_GFX_LIFETIME_FRAME)
 *     "store"       ui/store  retained node-store slots
 *     "arena"       ui/core   context arena bytes
 *     "atlas"       text/atlas rasterised glyph entries
 *     "scene"       scene-graph nodes
 *
 *   OPTICS_GFX_SCOPE_DEVICE, owned by material (ADR-0134) — per-material so a leak names
 *   the material rather than "a filter":
 *     "filter_mica"            material/material
 *     "filter_frosted"         material/material
 *     "filter_acrylic"         material/material
 *     "filter_liquid_glass"    material/material
 *     "filter_backdrop_layer"  material/material
 *
 *   OPTICS_GFX_SCOPE_PROCESS
 *     "fd"          window        file descriptors (dup/open/pipe/eventfd)
 *     "wl_proxy"    window        Wayland protocol objects
 *     "wl_buffer"   window        Wayland buffers (shm and dmabuf-backed)
 *     "vk_surface"  gfx/core   VkSurfaceKHR
 *
 * Adding a kind means editing this list, then tools/config/leak-lanes.toml. */

#ifdef __cplusplus
}
#endif

#endif /* OPTICS_GFX_OBSERVE_H */
