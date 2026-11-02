/*
 * flux/observe.h — the observation plane (ADR-0129).
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
 *     flux_observe_opts, and their symbols must be absent from a build
 *     that did not request them (INV-ARCH-LEDGER).
 *   - Reporting is observational: it never mutates resource state, so it
 *     stays usable under OOM, after device loss, and on a host that has
 *     never submitted a frame.
 */

#ifndef FLUX_OBSERVE_H
#define FLUX_OBSERVE_H

#include <flux/core.h>
#include <flux/observe_sites.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/*  Scopes                                                            */
/* ================================================================== */

/* flux_scope lives in <flux/core.h>: core.h owns the allocation entry points
 * that take it, and observe.h cannot be a prerequisite of core.h. */
/* How a scope's drain point is enforced. Declared per row, because a
 * frame-lifetime kind may legitimately live in the context scope (a per-frame
 * scratch buffer owned by the lens store, for instance). */
typedef enum flux_observe_lifetime : uint32_t {
    /* Must return to zero at the end of every frame. Asserted in debug
     * configurations at flux_frame_present / flux_frame_abandon. */
    FLUX_LIFETIME_FRAME = 0,
    /* Must return to zero when the owning context is released. */
    FLUX_LIFETIME_CONTEXT = 1,
    /* Must return to zero when the process exits; reported, never spun on. */
    FLUX_LIFETIME_PROCESS = 2,
} flux_observe_lifetime;

/* ================================================================== */
/*  Counters                                                          */
/* ================================================================== */

/* One accounted (kind, owner) pair. `live` is the only field a test may
 * assert on; `peak` and `bytes_*` exist so that high-water regressions
 * (ADR-0072) and fragmentation are never conflated with a leak. */
typedef struct flux_observe_counters {
    uint64_t live;          /* acquire - release; must reach 0 at drain */
    uint64_t peak;          /* high-water mark of `live` */
    uint64_t acquire_total; /* monotonic; survives a balanced pair */
    uint64_t release_total; /* monotonic */
    uint64_t bytes_live;    /* 0 for kinds that carry no size */
    uint64_t bytes_peak;
} flux_observe_counters;

/* One row of a report. Strings are library-owned literals; they outlive
 * the device. `name` is NULL unless FLUX_OBSERVE_NAMES was requested;
 * `site` is NULL unless FLUX_OBSERVE_SITES was requested. */
typedef struct flux_observe_entry {
    flux_scope scope;
    flux_observe_lifetime lifetime;
    const char *kind;  /* closed library vocabulary: "host", "vk_memory", … */
    const char *owner; /* subsystem: "flux/core", "lens/store", "iris/wayland" */
    const char *name;  /* newest named instance of this kind; NULL if off */
    uint32_t instance; /* that instance's id; 0 when naming is off */
    const char *site;  /* fixed site vocabulary, see <flux/observe_sites.h> */
    uint64_t live;
    uint64_t bytes_live;
} flux_observe_entry;

/* Structured return for every reporting entry point (api-design.md §1.4). */
typedef struct flux_observe_response {
    flux_result result;
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
} flux_observe_response;

/* ================================================================== */
/*  Capabilities (ADR-0129 decision 2)                                */
/* ================================================================== */

typedef enum flux_observe_capability : uint32_t {
    /* Counting. Always on; declared here so a caller can assert the set. */
    FLUX_OBSERVE_COUNTS = 1u << 0,
    /* Derive VkDebugUtils object names from the ledger instead of the
     * per-call-site string literals. */
    FLUX_OBSERVE_NAMES = 1u << 1,
    /* vkCmdBeginDebugUtilsLabelEXT / End around every recorded pass. */
    FLUX_OBSERVE_PASS_LABELS = 1u << 2,
    /* First-acquire (file, line) ring. Allocation-free; literals only. */
    FLUX_OBSERVE_SITES = 1u << 3,
} flux_observe_capability;

/* Chain onto flux_device_desc.next (FLUX_TYPE_OBSERVE_OPTS). */
typedef struct flux_observe_opts {
    flux_struct_type type; /* FLUX_TYPE_OBSERVE_OPTS */
    const void *next;
    uint32_t capabilities;  /* OR of flux_observe_capability */
    uint32_t name_capacity; /* bounded name/site rows; 0 = library default */
    flux_log_fn log;        /* NULL = no teardown report (tests read the ledger) */
    void *log_user;
} flux_observe_opts;

/* Library default for name_capacity when the caller passes 0. Sized for a
 * few hundred named objects and zero dynamic growth. */
#define FLUX_OBSERVE_DEFAULT_NAME_CAPACITY 256u

/* ================================================================== */
/*  Accounting (hot path)                                             */
/* ================================================================== */

/* Record one acquisition. `d` may be nullptr for FLUX_SCOPE_PROCESS, whose
 * rows are process-global by nature (a display connection or wakeup eventfd
 * exists before the device does); every other scope requires a device.
 * `kind` and `owner` must be string literals (the
 * ledger interns by pointer identity — no copying, no allocation). `bytes`
 * is 0 for kinds that carry no size. `lifetime` states the drain point.
 * Safe from any thread the device is usable on. Returns false only if the
 * kind table is full, in which case the ledger's `overflowed_kinds` is
 * incremented and a later report says so.
 *
 * `site` is the literal returned by flux_site_label(site_) or NULL. */
FLUX_API bool flux_observe_acquire(flux_device *d, flux_scope scope, flux_observe_lifetime lifetime,
                                   const char *kind, const char *owner, uint64_t bytes,
                                   const char *site);

/* Record one release. Same identity rules as acquire. A release with no
 * matching *live* acquisition is counted and logged (with the kind and
 * owner) instead of being dropped: an unbalanced release is a defect in
 * its own right and must not be able to masquerade as a balanced pair. */
FLUX_API void flux_observe_release(flux_device *d, flux_scope scope, const char *kind,
                                   const char *owner, uint64_t bytes);

/* Convenience wrappers. The only sanctioned way to count, so a call site
 * cannot forget the site argument.
 *
 * `site_` is a flux_site enum slot, not `__FILE__ ":" __LINE__`: a per-call-site
 * format string would land one literal per counted site in the shipped object,
 * which is exactly the "site machinery" a non-attribution build promises not to
 * carry. Use FLUX_SITE_NONE when the kind has no fixed creation site. */
#define FLUX_OBSERVE_ACQUIRE(d, scope, lifetime, kind, owner, bytes, site_)                        \
    flux_observe_acquire((d), (scope), (lifetime), (kind), (owner), (bytes), flux_site_label(site_))
#define FLUX_OBSERVE_RELEASE(d, scope, kind, owner, bytes)                                         \
    flux_observe_release((d), (scope), (kind), (owner), (bytes))

/* ================================================================== */
/*  Reporting                                                         */
/* ================================================================== */

typedef struct flux_observe_report_opts {
    flux_struct_type type; /* FLUX_TYPE_OBSERVE_REPORT_OPTS */
    const void *next;
    /* Caller-owned row buffer. Required. */
    flux_observe_entry *entries;
    uint32_t capacity;
    /* false (default): only rows whose live count is non-zero, i.e. the
     * rows that are leaking at this drain point. true: every inherited
     * (kind, owner) row, for diagnostics and soak trend output. */
    bool include_drained;
    /* Restrict to one scope; FLUX_SCOPE_COUNT means "all scopes". */
    flux_scope scope;
} flux_observe_report_opts;

/* Read the ledger without side effects. Never allocates, never waits on a
 * fence, never takes the allocator lock, so it stays valid under OOM,
 * after device loss, and on a device that never submitted a frame. */
/* `d` may be nullptr when `opts->scope` is FLUX_SCOPE_PROCESS. */
FLUX_NODISCARD FLUX_API flux_observe_response
flux_observe_report(const flux_device *d, const flux_observe_report_opts *opts);

/* Snapshot one (kind, owner) row. Used by leak lanes to assert an exact
 * delta around a create/release pair instead of a tolerance constant.
 * Returns FLUX_ERROR_OUT_OF_RANGE when the row was never interned. */
FLUX_NODISCARD FLUX_API flux_result flux_observe_read_counters(const flux_device *d,
                                                               flux_scope scope, const char *kind,
                                                               const char *owner,
                                                               flux_observe_counters *out);

/* Emit the drain report for `scope` through the configured logger. Called
 * by the library at the scope's drain point; also callable by a host that
 * wants a checkpoint. Rows with a non-zero live count are logged at WARN,
 * one line each, in the form:
 *
 *   observe: leak scope=device kind=vk_memory owner=flux/core live=2
 *            bytes=8388608 name=...
 *
 * The word "possible" never appears: a scope past its drain point either
 * drained or leaked (ADR-0129 decision 3). */
FLUX_API void flux_observe_report_emit(const flux_device *d, flux_scope scope);

/* Emit the process-scope report through the process-global logger (the last
 * flux_device to register one). Call from an app/host shutdown path, after
 * every context is gone. This is the only report a host that never created a
 * device can still produce — the reason fd and protocol leaks used to be
 * unattributable. */
FLUX_API void flux_observe_report_emit_process(void);

/* Frame-lifetime enforcement (ADR-0129 decision 4). Called at the frame
 * boundary in debug configurations; returns false and logs when a
 * FLUX_LIFETIME_FRAME row is still live. Wired into flux_frame_present /
 * flux_frame_abandon, not called directly by hosts. */
FLUX_NODISCARD FLUX_API bool flux_observe_frame_drain_check(const flux_device *d);

/* Labels actually handed to the driver since device creation (ADR-0133).
 * Zero when FLUX_OBSERVE_PASS_LABELS is off, attribution is not compiled in, or
 * VK_EXT_debug_utils is unavailable. Exists so a pass-label implementation can
 * be asserted on rather than assumed: before it, nothing in the tree could tell
 * whether a label had ever been emitted. */
FLUX_API uint64_t flux_device_labels_emitted(const flux_device *d);

/* ================================================================== */
/*  Scope conventions — the closed kind vocabulary (ADR-0129)          */
/* ================================================================== */

/* Kind strings are a closed, library-owned vocabulary. A host may not mint
 * new kinds casually: `tools/optics check leaks` enumerates them from the
 * tree and requires a leak lane per kind (ADR-0130). Owner strings name the
 * subsystem that owns the release, so a report row always answers "who
 * should have released this?" without a code search.
 *
 *   FLUX_SCOPE_DEVICE
 *     "host"        flux/core   device-owned host allocation
 *     "vk_memory"   flux/core   VkDeviceMemory (slab or dedicated)
 *     "vk_buffer"   flux/core   VkBuffer
 *     "vk_image"    flux/core   VkImage + image view
 *     "vk_sampler"  flux/core   VkSampler
 *     "vk_shader"   flux/core   VkShaderModule
 *     "vk_layout"   flux/core   VkPipelineLayout / VkDescriptorSetLayout
 *     "vk_pipeline" flux/core   VkPipeline
 *     "vk_cmd"      flux/core   VkCommandPool / VkCommandBuffer pool
 *     "vk_fence"    flux/core   VkFence / VkSemaphore
 *     "bindless"    flux/core   one bindless heap slot
 *
 *   FLUX_SCOPE_CONTEXT
 *     "frame"       lens/core   one UI frame (FLUX_LIFETIME_FRAME)
 *     "store"       lens/store  retained node-store slots
 *     "arena"       lens/core   context arena bytes
 *     "atlas"       glyph/atlas rasterised glyph entries
 *     "scene"       vista/scene scene-graph nodes
 *
 *   FLUX_SCOPE_DEVICE, owned by prism (ADR-0134) — per-material so a leak names
 *   the material rather than "a filter":
 *     "filter_mica"            prism/material
 *     "filter_frosted"         prism/material
 *     "filter_acrylic"         prism/material
 *     "filter_liquid_glass"    prism/material
 *     "filter_backdrop_layer"  prism/material
 *
 *   FLUX_SCOPE_PROCESS
 *     "fd"          iris        file descriptors (dup/open/pipe/eventfd)
 *     "wl_proxy"    iris        Wayland protocol objects
 *     "wl_buffer"   iris        Wayland buffers (shm and dmabuf-backed)
 *     "vk_surface"  flux/core   VkSurfaceKHR
 *
 * Adding a kind means editing this list, then tools/config/leak-lanes.toml. */

#ifdef __cplusplus
}
#endif

#endif /* FLUX_OBSERVE_H */
