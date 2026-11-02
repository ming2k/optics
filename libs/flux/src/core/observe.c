/*
 * observe.c — the observation plane (ADR-0129).
 *
 * Fixed-capacity allocation ledger. Everything here is designed around one
 * constraint: it must stay usable when the rest of the system has failed.
 * Concretely — no allocation after init, no allocation on the reporting path,
 * no allocator lock, no fence wait, no Vulkan call. A report taken under OOM,
 * after device loss, or on a device that never submitted a frame must produce
 * the same rows as a report taken at a calm teardown.
 *
 * Row resolution is by *content* hash of (scope, kind, owner), confirmed with
 * strcmp. Pointer identity would be cheaper but wrong: rows interned by
 * libflux must be readable by a test binary, a host, or a Rust binding that
 * passes its own copy of the same literals. That mistake would have made the
 * ledger write-only from outside the library.
 *
 * Publication is atomic. `row_count` is a release-published atomic; reporters
 * read it with an acquire load and never take the interning lock. That is what
 * lets reporting stay lock-free (a diagnostic that can block a frame is not a
 * diagnostic) while still guaranteeing a reporter never sees a row whose
 * kind/owner pointers are still NULL.
 *
 * The ledger never silently truncates: once FLUX_OBSERVE_MAX_ROWS is
 * exhausted, `overflowed_kinds` increments and every report says so. A partial
 * report that claims to be complete is worse than no report.
 */

#include "internal.h"

#include <flux/observe.h>
#include <flux/observe_sites.h>

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Creation-site vocabulary                                          */
/*                                                                    */
/*  Sites are an enum, not `__FILE__ ":" __LINE__`. A per-call-site     */
/*  format string puts one literal per counted site into the shipped    */
/*  object — five in a default flux build — which contradicts the       */
/*  promise that a non-attribution build carries no site machinery.    */
/*  The switch below emits exactly the labels that are referenced.     */
/* ------------------------------------------------------------------ */

const char *flux_site_label(flux_site site) {
    switch (site) {
    case FLUX_SITE_HOST_ALLOC:
        return "core/device.c:flux_internal_alloc";
    case FLUX_SITE_VK_MEMORY:
        return "core/vk_allocator.c:flux_vk_allocate";
    case FLUX_SITE_VK_IMAGE:
        return "canvas/image.c:flux_image_create";
    case FLUX_SITE_VK_BUFFER:
        return "core/buffer.c:flux_buffer_create";
    case FLUX_SITE_VK_SAMPLER:
        return "core/sampler.c:flux_sampler_create";
    case FLUX_SITE_VK_PIPELINE:
        return "core/graphics_pipeline.c:flux_graphics_pipeline_create";
    case FLUX_SITE_VK_TARGET:
        return "core/target.c:flux_target_create";
    case FLUX_SITE_BINDLESS:
        return "core/device.c:bindless slot";
    case FLUX_SITE_COUNT:
    case FLUX_SITE_NONE:
        break;
    }
    return nullptr;
}

/* ------------------------------------------------------------------ */
/*  Process-global ledger                                             */
/*                                                                    */
/*  FLUX_SCOPE_PROCESS rows cannot belong to a device: a display       */
/*  connection, a wakeup eventfd, or a Wayland proxy is created before */
/*  any device exists and must stay attributable when device bringup   */
/*  fails, and after the device is gone.                              */
/* ------------------------------------------------------------------ */

static flux_observe_state g_process_observe;
static atomic_bool g_process_observe_ready;

/* Ensures the process ledger's mutex exists. Idempotent and safe to race:
 * the interning lock is only ever taken after this returns true. */
static bool process_observe_ready(void) {
    if (atomic_load_explicit(&g_process_observe_ready, memory_order_acquire))
        return true;
    if (!flux_platform_mutex_init(&g_process_observe.lock))
        return false;
    g_process_observe.lock_initialized = true;
    g_process_observe.capabilities = (uint32_t)FLUX_OBSERVE_COUNTS;
    atomic_store_explicit(&g_process_observe_ready, true, memory_order_release);
    return true;
}

/* The ledger a call belongs to. NULL means "cannot count". */
static flux_observe_state *ledger_for(flux_device *d, flux_scope scope) {
    if (scope == FLUX_SCOPE_PROCESS) {
        if (!process_observe_ready())
            return nullptr;
        return &g_process_observe;
    }
    if (!d || !d->observe.lock_initialized)
        return nullptr;
    return &d->observe;
}

/* ------------------------------------------------------------------ */
/*  Row keys                                                          */
/* ------------------------------------------------------------------ */

/* FNV-1a over (scope, kind, owner). Content identity, not pointer identity,
 * so rows interned by libflux are readable by a test binary, a host, or a
 * binding that passes its own copy of the same literals. */
static uint64_t row_key_hash(flux_scope scope, const char *kind, const char *owner) {
    uint64_t h = UINT64_C(0xcbf29ce484222325);
    h = (h ^ (uint64_t)scope) * UINT64_C(0x100000001b3);
    for (const unsigned char *p = (const unsigned char *)kind; *p; ++p)
        h = (h ^ *p) * UINT64_C(0x100000001b3);
    h = (h ^ 0xffu) * UINT64_C(0x100000001b3); /* separator: kind vs owner */
    for (const unsigned char *p = (const unsigned char *)owner; *p; ++p)
        h = (h ^ *p) * UINT64_C(0x100000001b3);
    return h;
}

/* ------------------------------------------------------------------ */
/*  Row resolution                                                    */
/* ------------------------------------------------------------------ */

/* Resolve (scope, kind, owner) to a row, interning on first use. Returns
 * NULL when the table is full or the defensive checks fail. */
static flux_observe_row *row_resolve(flux_observe_state *s, flux_scope scope,
                                     flux_observe_lifetime lifetime, const char *kind,
                                     const char *owner, const char *site, bool *created) {
    if (created)
        *created = false;
    if (!s || !kind || !owner)
        return nullptr;
    if ((uint32_t)scope >= (uint32_t)FLUX_SCOPE_COUNT)
        return nullptr;
    if (!s->lock_initialized)
        return nullptr;

    const uint64_t key = row_key_hash(scope, kind, owner);

    flux_platform_mutex_lock(&s->lock);
    const uint32_t nrows = atomic_load_explicit(&s->row_count, memory_order_relaxed);
    flux_observe_row *row = nullptr;
    /* Linear scan over at most FLUX_OBSERVE_MAX_ROWS rows. The hash decides
     * mismatches with one integer compare; strcmp only runs on a hash hit,
     * which happens at most once per kind in the life of a device. */
    for (uint32_t i = 0; i < nrows; ++i) {
        flux_observe_row *r = &s->rows[i];
        if (r->key_hash != key || r->scope != scope)
            continue;
        if (strcmp(r->kind, kind) != 0 || strcmp(r->owner, owner) != 0)
            continue;
        row = r;
        break;
    }

    if (!row) {
        if (nrows >= FLUX_OBSERVE_MAX_ROWS) {
            atomic_fetch_add_explicit(&s->overflowed_kinds, 1u, memory_order_relaxed);
            flux_platform_mutex_unlock(&s->lock);
            return nullptr;
        }
        row = &s->rows[nrows];
        memset(row, 0, sizeof(*row));
        row->kind = kind;
        row->owner = owner;
        row->scope = scope;
        row->key_hash = key;
        row->lifetime = lifetime;
        row->site = site;
        /* Publish last, release-ordered. A reporter running concurrently walks
         * rows[0..row_count-1] without the interning lock and must never see a
         * row whose kind/owner pointers are still NULL — it would dereference
         * them in strcmp. `row_count` is the publication barrier. */
        atomic_store_explicit(&s->row_count, nrows + 1u, memory_order_release);
        if (created)
            *created = true;
    } else if (!row->site && site) {
        /* The first acquire that carried a site wins; later ones do not
         * overwrite, so the reported site stays stable across runs. */
        row->site = site;
    }
    flux_platform_mutex_unlock(&s->lock);
    return row;
}

/* Read-only resolution for reporting and release. No interning: a release for
 * a pair that was never acquired is a defect, and interning it would create a
 * phantom row that reads as balanced forever. */
static const flux_observe_row *row_find_const(const flux_observe_state *s, flux_scope scope,
                                              const char *kind, const char *owner) {
    const uint64_t key = row_key_hash(scope, kind, owner);
    const uint32_t nrows = atomic_load_explicit(&s->row_count, memory_order_acquire);
    for (uint32_t i = 0; i < nrows; ++i) {
        const flux_observe_row *r = &s->rows[i];
        if (r->key_hash != key || r->scope != scope)
            continue;
        if (strcmp(r->kind, kind) != 0 || strcmp(r->owner, owner) != 0)
            continue;
        return r;
    }
    return nullptr;
}

/* ------------------------------------------------------------------ */
/*  Init / destroy                                                    */
/* ------------------------------------------------------------------ */

/* Adopt the device's logger for the process ledger. The first device to
 * bring up a logger wins; a later device does not steal it, so a report
 * still finds its way out after that device is gone. */
static void process_observe_adopt_logger(const flux_observe_opts *opts, flux_log_fn fallback,
                                         void *fallback_user) {
    if (!atomic_load_explicit(&g_process_observe_ready, memory_order_acquire))
        return;
    if (g_process_observe.log)
        return;
    if (opts && opts->log) {
        g_process_observe.log = opts->log;
        g_process_observe.log_user = opts->log_user;
    } else if (fallback) {
        g_process_observe.log = fallback;
        g_process_observe.log_user = fallback_user;
    }
}

flux_result flux_observe_init(flux_device *d, const flux_observe_opts *opts) {
    flux_observe_state *s = &d->observe;
    memset(s, 0, sizeof(*s));
    atomic_init(&s->row_count, 0u);
    atomic_init(&s->next_instance, 0u);
    atomic_init(&s->overflowed_kinds, 0u);
    atomic_init(&s->unmatched_releases, 0u);

    if (!flux_platform_mutex_init(&s->lock)) {
        FLUX_FAIL(FLUX_ERROR_BACKEND_FAILURE, "observe ledger mutex init failed");
        return FLUX_ERROR_BACKEND_FAILURE;
    }
    s->lock_initialized = true;

    /* Counting is not optional (INV-ARCH-LEDGER): a device always has a
     * ledger, whether or not the caller chained options. */
    s->capabilities = (uint32_t)FLUX_OBSERVE_COUNTS;
    if (opts) {
        if (opts->type != FLUX_TYPE_OBSERVE_OPTS) {
            flux_platform_mutex_destroy(&s->lock);
            s->lock_initialized = false;
            FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "flux_observe_opts.type mismatch");
            return FLUX_ERROR_INVALID_ARGUMENT;
        }
        s->capabilities |= opts->capabilities;
        s->log = opts->log;
        s->log_user = opts->log_user;
    }

    /* The process ledger shares the logger so host-level (fd, protocol)
     * leaks are reportable after every device is gone. */
    process_observe_adopt_logger(opts, d->log, d->log_user);
    return FLUX_OK;
}

void flux_observe_destroy(flux_device *d) {
    flux_observe_state *s = &d->observe;
    if (!s->lock_initialized)
        return;
    flux_platform_mutex_destroy(&s->lock);
    s->lock_initialized = false;
}

/* ------------------------------------------------------------------ */
/*  Accounting                                                        */
/* ------------------------------------------------------------------ */

bool flux_observe_acquire(flux_device *d, flux_scope scope, flux_observe_lifetime lifetime,
                          const char *kind, const char *owner, uint64_t bytes, const char *site) {
    bool created = false;
    flux_observe_state *state = ledger_for(d, scope);
    flux_observe_row *r = row_resolve(state, scope, lifetime, kind, owner, site, &created);
    if (!r)
        return false;
    (void)created;

    uint64_t live = atomic_fetch_add_explicit(&r->live, 1u, memory_order_relaxed) + 1u;
    atomic_fetch_add_explicit(&r->acquire_total, 1u, memory_order_relaxed);

    /* Peak update: relaxed CAS loop. Contention here only costs a retry on a
     * counter nobody reads on the hot path. */
    uint64_t peak = atomic_load_explicit(&r->peak, memory_order_relaxed);
    while (live > peak && !atomic_compare_exchange_weak_explicit(
                              &r->peak, &peak, live, memory_order_relaxed, memory_order_relaxed)) {
        /* `peak` is refreshed by the failed exchange. */
    }

    if (bytes) {
        uint64_t bytes_live =
            atomic_fetch_add_explicit(&r->bytes_live, bytes, memory_order_relaxed) + bytes;
        uint64_t bytes_peak = atomic_load_explicit(&r->bytes_peak, memory_order_relaxed);
        while (bytes_live > bytes_peak &&
               !atomic_compare_exchange_weak_explicit(&r->bytes_peak, &bytes_peak, bytes_live,
                                                      memory_order_relaxed, memory_order_relaxed)) {
        }
    }
    return true;
}

void flux_observe_release(flux_device *d, flux_scope scope, const char *kind, const char *owner,
                          uint64_t bytes) {
    if (!kind || !owner)
        return;
    if ((uint32_t)scope >= (uint32_t)FLUX_SCOPE_COUNT)
        return;
    flux_observe_state *s = ledger_for(d, scope);
    if (!s)
        return;

    const flux_observe_row *found = row_find_const(s, scope, kind, owner);
    if (!found) {
        /* Cold, and already wrong: record it so the report cannot hide it. */
        atomic_fetch_add_explicit(&s->unmatched_releases, 1u, memory_order_relaxed);
        return;
    }
    flux_observe_row *r = (flux_observe_row *)found;

    uint64_t prev = atomic_fetch_sub_explicit(&r->live, 1u, memory_order_relaxed);
    if (prev == 0) {
        /* Underflow: restore and record. A double release must be visible,
         * not wrapped around into a huge uint64 that looks live forever. */
        atomic_fetch_add_explicit(&r->live, 1u, memory_order_relaxed);
        atomic_fetch_add_explicit(&s->unmatched_releases, 1u, memory_order_relaxed);
        return;
    }
    atomic_fetch_add_explicit(&r->release_total, 1u, memory_order_relaxed);

    if (bytes) {
        uint64_t prev_bytes =
            atomic_fetch_sub_explicit(&r->bytes_live, bytes, memory_order_relaxed);
        if (prev_bytes < bytes) {
            /* Same rule as the count: clamp instead of wrapping. */
            atomic_store_explicit(&r->bytes_live, 0u, memory_order_relaxed);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Reporting                                                         */
/* ------------------------------------------------------------------ */

/* Read every atomic for one row into caller-visible counters. */
static void row_snapshot(const flux_observe_row *r, flux_observe_entry *out) {
    out->scope = r->scope;
    out->lifetime = r->lifetime;
    out->kind = r->kind;
    out->owner = r->owner;
    /* The most recent live instance of this kind, or nullptr when naming is
     * off. A row aggregates instances, so "the name" only has meaning as "the
     * newest one"; a reader that needs a specific object asks by instance id. */
    out->name = r->name_top ? r->names[r->name_top - 1u].name : nullptr;
    out->instance = r->name_top ? r->names[r->name_top - 1u].id : 0u;
    out->site = r->site;
    out->live = atomic_load_explicit(&r->live, memory_order_relaxed);
    out->bytes_live = atomic_load_explicit(&r->bytes_live, memory_order_relaxed);
}

flux_observe_response flux_observe_report(const flux_device *d,
                                          const flux_observe_report_opts *opts) {
    flux_observe_response resp = {};
    resp.result = FLUX_OK;

    if (!opts || !opts->entries || opts->capacity == 0) {
        resp.result = FLUX_ERROR_INVALID_ARGUMENT;
        return resp;
    }
    if (opts->type != FLUX_TYPE_OBSERVE_REPORT_OPTS) {
        resp.result = FLUX_ERROR_INVALID_ARGUMENT;
        return resp;
    }

    /* Process-scope rows are ledger-global; every other scope needs a
     * device. A nullptr device with a non-process scope is an argument
     * error, not an empty report. */
    const flux_observe_state *s;
    if (opts->scope == FLUX_SCOPE_PROCESS) {
        if (!atomic_load_explicit(&g_process_observe_ready, memory_order_acquire)) {
            resp.result = FLUX_OK; /* nothing was ever counted */
            return resp;
        }
        s = &g_process_observe;
    } else {
        if (!d) {
            resp.result = FLUX_ERROR_INVALID_ARGUMENT;
            return resp;
        }
        s = &d->observe;
        if (!s->lock_initialized) {
            resp.result = FLUX_ERROR_INVALID_STATE;
            return resp;
        }
    }

    /* Deliberately lock-free: rows are append-only with immutable string
     * pointers, every counter is read atomically, and `row_count` is
     * release-published. A concurrent acquire can add a row mid-report; that
     * row simply does not appear, and the report stays internally consistent.
     * Taking the interning lock here would make a diagnostic able to block a
     * frame. */
    const uint32_t nrows = atomic_load_explicit(&s->row_count, memory_order_acquire);
    for (uint32_t i = 0; i < nrows; ++i) {
        const flux_observe_row *r = &s->rows[i];
        if (opts->scope != FLUX_SCOPE_COUNT && opts->scope != r->scope)
            continue;

        flux_observe_entry entry = {};
        row_snapshot(r, &entry);
        resp.live_total += entry.live;
        if (!opts->include_drained && entry.live == 0)
            continue;
        if (resp.count >= opts->capacity) {
            resp.total++;
            continue;
        }
        opts->entries[resp.count++] = entry;
        resp.total++;
    }

    resp.overflowed_kinds = atomic_load_explicit(&s->overflowed_kinds, memory_order_relaxed);
    return resp;
}

flux_result flux_observe_read_counters(const flux_device *d, flux_scope scope, const char *kind,
                                       const char *owner, flux_observe_counters *out) {
    if (!kind || !owner || !out)
        return FLUX_ERROR_INVALID_ARGUMENT;
    const flux_observe_state *s;
    if (scope == FLUX_SCOPE_PROCESS) {
        if (!atomic_load_explicit(&g_process_observe_ready, memory_order_acquire))
            return FLUX_ERROR_OUT_OF_RANGE;
        s = &g_process_observe;
    } else {
        if (!d)
            return FLUX_ERROR_INVALID_ARGUMENT;
        s = &d->observe;
        if (!s->lock_initialized)
            return FLUX_ERROR_INVALID_STATE;
    }

    const flux_observe_row *r = row_find_const(s, scope, kind, owner);
    if (!r)
        return FLUX_ERROR_OUT_OF_RANGE;

    *out = (flux_observe_counters){
        .live = atomic_load_explicit(&r->live, memory_order_relaxed),
        .peak = atomic_load_explicit(&r->peak, memory_order_relaxed),
        .acquire_total = atomic_load_explicit(&r->acquire_total, memory_order_relaxed),
        .release_total = atomic_load_explicit(&r->release_total, memory_order_relaxed),
        .bytes_live = atomic_load_explicit(&r->bytes_live, memory_order_relaxed),
        .bytes_peak = atomic_load_explicit(&r->bytes_peak, memory_order_relaxed),
    };
    return FLUX_OK;
}

void flux_observe_report_emit(const flux_device *d, flux_scope scope) {
    if ((uint32_t)scope >= (uint32_t)FLUX_SCOPE_COUNT)
        return;
    const flux_observe_state *s;
    if (scope == FLUX_SCOPE_PROCESS) {
        if (!atomic_load_explicit(&g_process_observe_ready, memory_order_acquire))
            return;
        s = &g_process_observe;
    } else {
        if (!d)
            return;
        s = &d->observe;
    }
    if (!s->lock_initialized || !s->log)
        return;

    /* One fixed stack buffer per line. No allocation: this runs at teardown,
     * possibly during OOM unwind. */
    char buf[320];
    const uint32_t nrows = atomic_load_explicit(&s->row_count, memory_order_acquire);
    for (uint32_t i = 0; i < nrows; ++i) {
        const flux_observe_row *r = &s->rows[i];
        if (r->scope != scope)
            continue;
        uint64_t live = atomic_load_explicit(&r->live, memory_order_relaxed);
        uint64_t bytes = atomic_load_explicit(&r->bytes_live, memory_order_relaxed);
        if (live == 0 && bytes == 0)
            continue;

        /* No "possible" hedge: this row is past its scope's drain point, so
         * it is a leak, not a suspicion (ADR-0129 decision 3). */
        snprintf(buf, sizeof(buf), "leak scope=%u kind=%s owner=%s live=%llu bytes=%llu site=%s",
                 (unsigned)r->scope, r->kind, r->owner, (unsigned long long)live,
                 (unsigned long long)bytes, r->site ? r->site : "-");
        s->log(FLUX_LOG_WARN, "observe", 0, "%s", buf, s->log_user);
    }

    uint64_t overflowed = atomic_load_explicit(&s->overflowed_kinds, memory_order_relaxed);
    if (overflowed) {
        snprintf(buf, sizeof(buf),
                 "ledger incomplete: %llu kind(s) refused past capacity %u; "
                 "this report cannot be read as complete",
                 (unsigned long long)overflowed, (unsigned)FLUX_OBSERVE_MAX_ROWS);
        s->log(FLUX_LOG_ERROR, "observe", 0, "%s", buf, s->log_user);
    }
    uint64_t unmatched = atomic_load_explicit(&s->unmatched_releases, memory_order_relaxed);
    if (unmatched) {
        snprintf(buf, sizeof(buf),
                 "%llu release(s) had no live acquisition (double release or "
                 "missing acquire)",
                 (unsigned long long)unmatched);
        s->log(FLUX_LOG_ERROR, "observe", 0, "%s", buf, s->log_user);
    }
}

void flux_observe_report_emit_process(void) {
    if (!atomic_load_explicit(&g_process_observe_ready, memory_order_acquire))
        return;
    flux_observe_report_emit(nullptr, FLUX_SCOPE_PROCESS);
}

/* Frame-lifetime enforcement (ADR-0129 decision 4).
 *
 * Always compiled, deliberately. An earlier form wrapped this in
 * `#if defined(FLUX_DEBUG_OBSERVE)` — a macro no build defined, so the check
 * was a `return true` stub and FLUX_LIFETIME_FRAME had no enforcement at all.
 * That is exactly the dead-branch hazard this ADR set out to remove (the atlas
 * leak lane spent a release cycle behind a similarly undefined macro), so the
 * branch is gone rather than given a define.
 *
 * Cost at a frame boundary: one compare per ledger row, and only rows declared
 * FLUX_LIFETIME_FRAME are examined. Nothing is formatted unless a violation has
 * already happened. */
bool flux_observe_frame_drain_check(const flux_device *d) {
    if (!d)
        return true;
    const flux_observe_state *s = &d->observe;
    if (!s->lock_initialized)
        return true;

    bool clean = true;
    const uint32_t nrows = atomic_load_explicit(&s->row_count, memory_order_acquire);
    for (uint32_t i = 0; i < nrows; ++i) {
        const flux_observe_row *r = &s->rows[i];
        if (r->lifetime != FLUX_LIFETIME_FRAME)
            continue;
        uint64_t live = atomic_load_explicit(&r->live, memory_order_relaxed);
        if (live == 0)
            continue;
        clean = false;
        if (s->log) {
            char buf[256];
            snprintf(buf, sizeof(buf),
                     "frame-scope leak: kind=%s owner=%s live=%llu (no release before "
                     "the frame boundary)",
                     r->kind, r->owner, (unsigned long long)live);
            s->log(FLUX_LOG_ERROR, "observe", 0, "%s", buf, s->log_user);
        }
    }
    return clean;
}

/* ------------------------------------------------------------------ */
/*  Object naming (ADR-0132)                                          */
/*                                                                    */
/*  A name is derived from the ledger, rendered into the row's own     */
/*  storage, and handed to the driver from there. Rows outlive their   */
/*  objects; the ledger is destroyed after the device. That is what    */
/*  makes the pointer safe, in place of a per-call-site stack buffer   */
/*  which the driver is merely *expected* to copy.                    */
/* ------------------------------------------------------------------ */

#if defined(FLUX_OBSERVE_ATTRIBUTION)
/* Render "<kind>#<instance>" into its own ring slot and return it. Each slot
 * owns its storage, so a later instance never invalidates an earlier name. */
static const char *name_render(flux_observe_row *r, uint32_t slot, uint32_t instance) {
    snprintf(r->names[slot].name, sizeof(r->names[slot].name), "%s#%u", r->kind, instance);
    return r->names[slot].name;
}
#endif

const char *flux_observe_name_object(flux_device *d, flux_scope scope, const char *kind,
                                     const char *owner) {
#if !defined(FLUX_OBSERVE_ATTRIBUTION)
    (void)d;
    (void)scope;
    (void)kind;
    (void)owner;
    return nullptr;
#else
    if (!d || !kind || !owner)
        return nullptr;
    flux_observe_state *s = &d->observe;
    if (!s->lock_initialized)
        return nullptr;
    if ((s->capabilities & (uint32_t)FLUX_OBSERVE_NAMES) == 0)
        return nullptr;

    flux_observe_row *r =
        row_resolve(s, scope, FLUX_LIFETIME_CONTEXT, kind, owner, nullptr, nullptr);
    if (!r)
        return nullptr;

    uint32_t instance = atomic_fetch_add_explicit(&s->next_instance, 1u, memory_order_relaxed) + 1u;

    flux_platform_mutex_lock(&s->lock);
    uint32_t slot;
    if (r->name_top == FLUX_OBSERVE_NAME_RING) {
        /* Ring full: recycle the oldest slot. Counted, never silent. */
        r->name_overflow++;
        memmove(&r->names[0], &r->names[1], sizeof(r->names[0]) * (FLUX_OBSERVE_NAME_RING - 1u));
        slot = FLUX_OBSERVE_NAME_RING - 1u;
    } else {
        slot = r->name_top++;
    }
    r->names[slot].id = instance;
    const char *name = name_render(r, slot, instance);
    flux_platform_mutex_unlock(&s->lock);
    return name;
#endif
}

uint32_t flux_observe_instance_of(flux_device *d, flux_scope scope, const char *kind,
                                  const char *owner) {
#if !defined(FLUX_OBSERVE_ATTRIBUTION)
    (void)d;
    (void)scope;
    (void)kind;
    (void)owner;
    return 0;
#else
    if (!d || !kind || !owner)
        return 0;
    const flux_observe_state *s = &d->observe;
    if (!s->lock_initialized)
        return 0;
    const flux_observe_row *r = row_find_const(s, scope, kind, owner);
    if (!r || r->name_top == 0)
        return 0;
    return r->names[r->name_top - 1u].id;
#endif
}
