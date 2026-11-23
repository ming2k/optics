/* wayland_data.c — Wayland data transfer: clipboard (wl_data_device), primary
 * selection, asynchronous offer reads and drag-and-drop.
 *
 * Everything here answers to the seat owned by app_wayland.c and delivers its
 * results to the UI host on the window main thread. */

#include "wayland_internal.h"

/* ------------------------------------------------------------------ */
/*  Clipboard (wl_data_device) — bridges optics_ui_clipboard to the selection */
/* ------------------------------------------------------------------ */

/* Offers announce their MIME types via offer events before the
 * selection/enter event binds them; accumulate into the pending slot and
 * move the record when the offer is bound (ddev_selection / ddev_enter). */
static struct wp_offer_mimes *offer_mime_slot(wp_platform *pl, struct wl_data_offer *off) {
    if (pl->pending_offer_mimes.offer == off)
        return &pl->pending_offer_mimes;
    if (pl->selection_offer_mimes.offer == off)
        return &pl->selection_offer_mimes;
    if (pl->dnd_offer_mimes.offer == off)
        return &pl->dnd_offer_mimes;
    return nullptr;
}

static void doffer_offer(void *data, struct wl_data_offer *off, const char *mime) {
    wp_platform *pl = data;
    struct wp_offer_mimes *slot = offer_mime_slot(pl, off);
    if (!slot)
        return;
    if (strcmp(mime, "text/uri-list") == 0)
        slot->uri_list = true;
    else if (strcmp(mime, "x-special/gnome-copied-files") == 0)
        slot->gnome_copied_files = true;
    else if (strcmp(mime, "text/plain;charset=utf-8") == 0)
        slot->text_utf8 = true;
    else if (strcmp(mime, "text/plain") == 0)
        slot->text_plain = true;
}
static void doffer_source_actions(void *d, struct wl_data_offer *o, uint32_t a) {
    (void)d;
    (void)o;
    (void)a;
}
static void doffer_action(void *d, struct wl_data_offer *o, uint32_t a) {
    (void)d;
    (void)o;
    (void)a;
}
static const struct wl_data_offer_listener data_offer_listener = {
    .offer = doffer_offer,
    .source_actions = doffer_source_actions,
    .action = doffer_action,
};

/* The compositor announces a new offer object before selection/enter. */
static void ddev_data_offer(void *data, struct wl_data_device *dev, struct wl_data_offer *offer) {
    wp_platform *pl = data;
    (void)dev;
    /* A previous offer that was announced but never bound (no selection /
     * enter referenced it) is ours to destroy, or it leaks. */
    if (pl->pending_offer_mimes.offer && pl->pending_offer_mimes.offer != offer)
        wl_data_offer_destroy(pl->pending_offer_mimes.offer);
    pl->pending_offer_mimes = (struct wp_offer_mimes){.offer = offer,
                                                      .uri_list = false,
                                                      .text_utf8 = false,
                                                      .text_plain = false,
                                                      .gnome_copied_files = false};
    wl_data_offer_add_listener(offer, &data_offer_listener, pl);
}
static void ddev_selection(void *data, struct wl_data_device *dev, struct wl_data_offer *offer) {
    wp_platform *pl = data;
    (void)dev;
    if (pl->selection_offer && pl->selection_offer != offer)
        wl_data_offer_destroy(pl->selection_offer);
    pl->selection_offer = offer; /* may be NULL when selection is cleared */
    pl->selection_offer_mimes = (struct wp_offer_mimes){.offer = offer};
    if (offer && pl->pending_offer_mimes.offer == offer) {
        pl->selection_offer_mimes = pl->pending_offer_mimes;
        pl->pending_offer_mimes = (struct wp_offer_mimes){};
    }
}

/* Strongest readable text MIME on the offer, or NULL when it carries no
 * text we can use (image-only drags, …). uri-list is preferred for file
 * drags; utf-8 text beats legacy text/plain. */
static const char *offer_pick_mime(const struct wp_offer_mimes *m) {
    if (m->gnome_copied_files)
        return "x-special/gnome-copied-files";
    if (m->uri_list)
        return "text/uri-list";
    if (m->text_utf8)
        return "text/plain;charset=utf-8";
    if (m->text_plain)
        return "text/plain";
    return nullptr;
}

/* Read a received offer fd with a hard overall deadline. Returns a malloc'd
 * buffer (caller frees) and its length via *out_len; NULL on error/timeout.
 * Runs on the async reader thread for every path (paste, primary paste,
 * drop) — never on the UI thread (cross-platform.md invariant 4). */
#define WP_DND_READ_TIMEOUT_MS 2000
#define WP_DROP_MAX (1u << 20) /* 1 MiB — matches ui's paste staging cap */

static char *read_offer_fd(int fd, int timeout_ms, size_t *out_len) {
    *out_len = 0;
    char *out = nullptr;
    size_t total = 0;
    long long deadline = now_ns() + (long long)timeout_ms * 1000000LL;
    for (;;) {
        long long remain_ms = (deadline - now_ns()) / 1000000LL;
        if (remain_ms <= 0)
            break; /* deadline: a hostile or stuck source cannot hang the UI */
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        int pr = poll(&pfd, 1, (int)(remain_ms > 100 ? 100 : remain_ms));
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (pr == 0)
            continue; /* re-check the deadline */
        char chunk[16384];
        ssize_t r = read(fd, chunk, sizeof chunk);
        if (r == 0)
            break; /* EOF */
        if (r < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (total + (size_t)r > WP_DROP_MAX) {
            fprintf(stderr, "window: drop/paste payload exceeds %u bytes; truncated\n",
                    (unsigned)WP_DROP_MAX);
            size_t room = WP_DROP_MAX - total;
            if (room) {
                char *grown = realloc(out, WP_DROP_MAX);
                if (!grown) {
                    free(out);
                    return nullptr;
                }
                out = grown;
                memcpy(out + total, chunk, room);
                total = WP_DROP_MAX;
            }
            break;
        }
        char *grown = realloc(out, total + (size_t)r);
        if (!grown) {
            free(out);
            return nullptr;
        }
        out = grown;
        memcpy(out + total, chunk, (size_t)r);
        total += (size_t)r;
    }
    *out_len = total;
    return out;
}

/* ------------------------------------------------------------------ */
/*  Async offer reads (clipboard paste / primary paste / DND drop)       */
/* ------------------------------------------------------------------ */

/* cross-platform.md invariant 4: the UI thread never blocks reading a
 * selection or drop payload — a stuck or malicious source must not hang
 * the app. Every read (clipboard paste, middle-click primary paste, DND
 * drop) runs on a detached helper thread with a hard deadline, and
 * completion is posted through optics_window_post_to_main_thread — which also makes
 * the self-paste case correct: the main thread keeps dispatching, so our
 * own data source can answer. */
#define WP_PASTE_TIMEOUT_MS 5000

typedef struct wp_paste_job {
    wp_platform *pl;
    int fd;
    struct wl_data_offer *offer; /* DND drop only: finish+destroy on delivery */
    uint32_t timeout_ms;
    bool strip_crlf; /* strip trailing \\r\\n (uri-list drops) */
} wp_paste_job;

typedef struct wp_paste_result {
    wp_platform *pl;
    char *text; /* NULL when the read failed or timed out */
    size_t len;
    struct wl_data_offer *offer;
} wp_paste_result;

/* Runs on the window main thread via optics_window_post_to_main_thread. pl->ui is
 * checked (not just pl): teardown clears it before draining the queue. */
static void paste_deliver(void *user) {
    wp_paste_result *res = user;
    /* A drop's offer stays alive until the read completes so we can tell
     * the source the transfer finished (protocol: finish before destroy).
     * The slot check guards against teardown having destroyed it already. */
    if (res->offer && res->pl->dnd_job_offer == res->offer) {
        wl_data_offer_finish(res->offer);
        wl_data_offer_destroy(res->offer);
        res->pl->dnd_job_offer = nullptr;
    }
    if (res->pl->ui && res->text && res->len) {
        optics_window__ui_paste(res->pl->ui, res->text, res->len);
        optics_window__ui_drop(res->pl->ui, res->text, res->len, res->pl->dnd_pos);
    }
    free(res->text);
    free(res);
}

static void *paste_reader_main(void *arg) {
    wp_paste_job *job = arg;
    size_t len = 0;
    char *text = read_offer_fd(job->fd, (int)job->timeout_ms, &len);
    optics_window_fd_close_record(job->fd, "fd_pipe_read");
    if (text && job->strip_crlf) {
        /* uri-list payloads are CRLF-terminated; the terminator is not
         * part of the dropped text. */
        while (len && (text[len - 1] == '\n' || text[len - 1] == '\r'))
            text[--len] = '\0';
    }

    wp_paste_result *res = malloc(sizeof *res);
    if (res) {
        res->pl = job->pl;
        res->text = text;
        res->len = len;
        res->offer = job->offer;
        /* -1: the loop is gone (window closed mid-read) — drop. A DND
         * offer in flight is then finished/destroyed by teardown (it owns
         * the dnd_job_offer slot). */
        if (optics_window_post_to_main_thread(paste_deliver, res) != 0) {
            free(text);
            free(res);
        }
    } else {
        free(text);
    }
    free(job);
    return nullptr;
}

/* Spawn the detached reader for an offer pipe's read end. Returns false on
 * failure (fd closed by caller path). */
static bool start_offer_read(wp_platform *pl, int fd, struct wl_data_offer *offer,
                             uint32_t timeout_ms, bool strip_crlf) {
    wp_paste_job *job = malloc(sizeof *job);
    if (!job)
        return false;
    job->pl = pl;
    job->fd = fd;
    job->offer = offer;
    job->timeout_ms = timeout_ms;
    job->strip_crlf = strip_crlf;
    pthread_t th;
    if (pthread_create(&th, nullptr, paste_reader_main, job) != 0) {
        free(job);
        return false;
    }
    pthread_detach(th);
    return true;
}

static void ddev_enter(void *d, struct wl_data_device *dev, uint32_t s, struct wl_surface *su,
                       wl_fixed_t x, wl_fixed_t y, struct wl_data_offer *o) {
    (void)dev;
    (void)su;
    wp_platform *pl = d;
    pl->dnd_inside = true;
    pl->dnd_pos = (optics_gfx_point){wl_fixed_to_double(x), wl_fixed_to_double(y)};
    (void)wp_emit_dnd(pl, pl->dnd_pos.x, pl->dnd_pos.y, true);
    if (pl->dnd_offer != o) {
        if (pl->dnd_offer)
            wl_data_offer_destroy(pl->dnd_offer);
        pl->dnd_offer = o;
        pl->dnd_offer_mimes = (struct wp_offer_mimes){.offer = o};
        if (o && pl->pending_offer_mimes.offer == o) {
            pl->dnd_offer_mimes = pl->pending_offer_mimes;
            pl->pending_offer_mimes = (struct wp_offer_mimes){};
        }
    }
    if (o) {
        /* Real MIME negotiation: accept only when the drag carries a text
         * type we can read; otherwise leave the offer un-accepted and the
         * compositor shows the not-allowed cursor. */
        const char *mime = offer_pick_mime(&pl->dnd_offer_mimes);
        if (mime) {
            wl_data_offer_accept(o, s, mime);
            wl_data_offer_set_actions(o, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY,
                                      WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
        }
    }
}
static void ddev_leave(void *d, struct wl_data_device *dev) {
    (void)dev;
    wp_platform *pl = d;
    pl->dnd_inside = false;
    (void)wp_emit_dnd(pl, pl->dnd_pos.x, pl->dnd_pos.y, false);
    if (pl->dnd_offer) {
        wl_data_offer_destroy(pl->dnd_offer);
        pl->dnd_offer = nullptr;
        pl->dnd_offer_mimes = (struct wp_offer_mimes){};
    }
}
static void ddev_motion(void *d, struct wl_data_device *dev, uint32_t t, wl_fixed_t x,
                        wl_fixed_t y) {
    (void)dev;
    (void)t;
    wp_platform *pl = d;
    pl->dnd_pos = (optics_gfx_point){wl_fixed_to_double(x), wl_fixed_to_double(y)};
    (void)wp_emit_dnd(pl, pl->dnd_pos.x, pl->dnd_pos.y, true);
}
static void ddev_drop(void *d, struct wl_data_device *dev) {
    (void)dev;
    wp_platform *pl = d;
    if (pl->dnd_offer) {
        /* The read runs off the UI thread (invariant 4): hand the offer to
         * the async job slot; delivery (paste + finish + destroy) happens
         * in paste_deliver on the main thread when the read completes. */
        const char *mime = offer_pick_mime(&pl->dnd_offer_mimes);
        bool started = false;
        if (mime && pl->ui && !pl->dnd_job_offer) {
            int fds[2];
            if (pipe(fds) == 0) {
                optics_window_fd_open_record(fds[1], "fd_pipe_write");
                optics_window_fd_open_record(fds[0], "fd_pipe_read");
                wl_data_offer_receive(pl->dnd_offer, mime, fds[1]);
                optics_window_fd_close_record(fds[1], "fd_pipe_write");
                wl_display_flush(pl->display);
                started = start_offer_read(pl, fds[0], pl->dnd_offer, WP_DND_READ_TIMEOUT_MS,
                                           true /* uri-list CRLF strip */);
                if (!started)
                    optics_window_fd_close_record(fds[0], "fd_pipe_read");
                else
                    pl->dnd_job_offer = pl->dnd_offer;
            }
        }
        if (!started) {
            /* No usable payload or no reader: close the DND session now. */
            wl_data_offer_finish(pl->dnd_offer);
            wl_data_offer_destroy(pl->dnd_offer);
        }
        pl->dnd_offer = nullptr;
        pl->dnd_offer_mimes = (struct wp_offer_mimes){};
    }
    pl->dnd_inside = false;
}
static const struct wl_data_device_listener data_device_listener = {
    .data_offer = ddev_data_offer,
    .enter = ddev_enter,
    .leave = ddev_leave,
    .motion = ddev_motion,
    .drop = ddev_drop,
    .selection = ddev_selection,
};

/* Our outgoing selection: write the stored buffer when a reader asks. */
static void dsource_send(void *data, struct wl_data_source *src, const char *mime, int32_t fd) {
    wp_platform *pl = data;
    (void)src;
    /* The compositor hands the destination's pipe write end to us; it is
     * ours to close on every path (adopted here, released by the wrapper). */
    optics_window_fd_open_record(fd, "fd_selection_write");
    const char *p = pl->copy_buf;
    size_t left = pl->copy_len;

    if (p && mime && strcmp(mime, "text/uri-list") == 0) {
        if (left >= 5 && strncmp(p, "copy\n", 5) == 0) {
            p += 5;
            left -= 5;
        } else if (left >= 4 && strncmp(p, "cut\n", 4) == 0) {
            p += 4;
            left -= 4;
        }
    }

    while (left) {
        ssize_t w = write(fd, p, left);
        if (w <= 0)
            break;
        p += w;
        left -= (size_t)w;
    }
    optics_window_fd_close_record(fd, "fd_selection_write");
}
static void dsource_cancelled(void *data, struct wl_data_source *src) {
    wp_platform *pl = data;
    wl_data_source_destroy(src);
    if (pl->copy_source == src)
        pl->copy_source = nullptr;
}
static void dsource_target(void *d, struct wl_data_source *s, const char *m) {
    (void)d;
    (void)s;
    (void)m;
}
/* v3 events. A selection source is never a drag source, so these cannot
 * legitimately arrive — but libwayland aborts the process when ANY event lands
 * on a NULL listener slot, and the data_device_manager is bound at v3, which
 * makes them dispatchable. No-op beats a crash on a compositor that answers
 * loosely. */
static void dsource_dnd_drop_performed(void *d, struct wl_data_source *s) {
    (void)d;
    (void)s;
}
static void dsource_dnd_finished(void *d, struct wl_data_source *s) {
    (void)d;
    (void)s;
}
static void dsource_action(void *d, struct wl_data_source *s, uint32_t action) {
    (void)d;
    (void)s;
    (void)action;
}
static const struct wl_data_source_listener data_source_listener = {
    .target = dsource_target,
    .send = dsource_send,
    .cancelled = dsource_cancelled,
    .dnd_drop_performed = dsource_dnd_drop_performed,
    .dnd_finished = dsource_dnd_finished,
    .action = dsource_action,
};

/* Outgoing Drag-and-Drop source (ADR-0086) */
static void drag_source_cleanup(wp_platform *pl) {
    if (pl->drag_source) {
        wl_data_source_destroy(pl->drag_source);
        pl->drag_source = nullptr;
    }
    if (pl->drag_text_buf) {
        free(pl->drag_text_buf);
        pl->drag_text_buf = nullptr;
        pl->drag_text_len = 0;
    }
    pl->drag_active = false;
    pl->dnd_inside = false;
    pl->acc.down[0] = false;
    pl->acc.released[0] = true;
    memset(&pl->drag_info, 0, sizeof(pl->drag_info));
}

static void drag_source_send(void *data, struct wl_data_source *src, const char *mime, int32_t fd) {
    wp_platform *pl = data;
    (void)src;
    optics_window_fd_open_record(fd, "fd_drag_write");
    /* A host callback that owns the fd still goes through the wrapper so the
     * row balances; ownership of the descriptor itself is unchanged. */
    if (pl->drag_info.callbacks.provide_data) {
        pl->drag_info.callbacks.provide_data(mime, fd, pl->drag_info.callbacks.user);
    } else if (pl->drag_text_buf && pl->drag_text_len) {
        const char *p = pl->drag_text_buf;
        size_t left = pl->drag_text_len;
        if (p && mime && strcmp(mime, "text/uri-list") == 0 && p[0] == '/') {
            char uri[4096];
            int n = snprintf(uri, sizeof(uri), "file://%s\r\n", p);
            if (n > 0 && (size_t)n < sizeof(uri)) {
                ssize_t w = write(fd, uri, (size_t)n);
                (void)w;
                optics_window_fd_close_record(fd, "fd_drag_write");
                return;
            }
        }
        while (left) {
            ssize_t w = write(fd, p, left);
            if (w <= 0)
                break;
            p += w;
            left -= (size_t)w;
        }
        optics_window_fd_close_record(fd, "fd_drag_write");
    } else {
        optics_window_fd_close_record(fd, "fd_drag_write");
    }
}

static void drag_source_cancelled(void *data, struct wl_data_source *src) {
    wp_platform *pl = data;
    (void)src;
    if (pl->drag_info.callbacks.cancelled) {
        pl->drag_info.callbacks.cancelled(pl->drag_info.callbacks.user);
    }
    drag_source_cleanup(pl);
}

static void drag_source_target(void *d, struct wl_data_source *s, const char *m) {
    (void)d;
    (void)s;
    (void)m;
}

static void drag_source_dnd_drop_performed(void *data, struct wl_data_source *src) {
    (void)data;
    (void)src;
}

static void drag_source_dnd_finished(void *data, struct wl_data_source *src) {
    wp_platform *pl = data;
    (void)src;
    if (pl->drag_info.callbacks.finished) {
        pl->drag_info.callbacks.finished(OPTICS_WINDOW_DND_ACTION_COPY,
                                         pl->drag_info.callbacks.user);
    }
    drag_source_cleanup(pl);
}

static void drag_source_action(void *data, struct wl_data_source *src, uint32_t dnd_action) {
    (void)data;
    (void)src;
    (void)dnd_action;
}

static const struct wl_data_source_listener drag_source_listener = {
    .target = drag_source_target,
    .send = drag_source_send,
    .cancelled = drag_source_cancelled,
    .dnd_drop_performed = drag_source_dnd_drop_performed,
    .dnd_finished = drag_source_dnd_finished,
    .action = drag_source_action,
};

OPTICS_WINDOW_API int optics_window_dnd_start(const optics_window_dnd_source *source) {
    wp_platform *pl = g_active_pl;
    if (!pl || !pl->data_device_mgr || !pl->data_device || !pl->surface || !source)
        return -1;

    if (pl->drag_active)
        drag_source_cleanup(pl);

    pl->drag_source = wl_data_device_manager_create_data_source(pl->data_device_mgr);
    if (!pl->drag_source)
        return -1;

    pl->drag_info = *source;
    pl->drag_active = true;

    if (source->static_text && source->static_text_len) {
        pl->drag_text_buf = malloc(source->static_text_len + 1);
        if (pl->drag_text_buf) {
            memcpy(pl->drag_text_buf, source->static_text, source->static_text_len);
            pl->drag_text_buf[source->static_text_len] = '\0';
            pl->drag_text_len = source->static_text_len;
        }
    }

    wl_data_source_add_listener(pl->drag_source, &drag_source_listener, pl);

    if (source->mime_count > 0 && source->mime_types) {
        for (uint32_t i = 0; i < source->mime_count; i++) {
            if (source->mime_types[i])
                wl_data_source_offer(pl->drag_source, source->mime_types[i]);
        }
    } else {
        const char *t = source->static_text;
        size_t tlen = source->static_text_len;
        if (t && tlen > 0 && t[0] == '/') {
            wl_data_source_offer(pl->drag_source, "text/uri-list");
        }
        wl_data_source_offer(pl->drag_source, "text/plain;charset=utf-8");
        wl_data_source_offer(pl->drag_source, "text/plain");
        wl_data_source_offer(pl->drag_source, "UTF8_STRING");
    }

    uint32_t actions =
        WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY | WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE;
    if (wl_data_source_get_version(pl->drag_source) >= 3) {
        wl_data_source_set_actions(pl->drag_source, actions);
    }

    wl_data_device_start_drag(pl->data_device, pl->drag_source, pl->surface, nullptr,
                              pl->last_serial);
    return 0;
}

OPTICS_WINDOW_API bool optics_window_dnd_is_active(void) {
    wp_platform *pl = g_active_pl;
    return pl && pl->drag_active;
}

OPTICS_WINDOW_API void optics_window_dnd_cancel(void) {
    wp_platform *pl = g_active_pl;
    if (pl && pl->drag_active) {
        drag_source_cleanup(pl);
    }
}

void wp_maybe_create_data_device(wp_platform *pl) {
    if (pl->data_device || !pl->data_device_mgr || !pl->seat)
        return;
    pl->data_device = wl_data_device_manager_get_data_device(pl->data_device_mgr, pl->seat);
    wl_data_device_add_listener(pl->data_device, &data_device_listener, pl);
}

/* ------------------------------------------------------------------ */
/*  Primary selection (zwp_primary_selection_unstable_v1)                */
/* ------------------------------------------------------------------ */

/* The X11-style middle-click clipboard, mirrored from wp_clip_set_text and
 * read on middle-button press (primsel_paste, called from ptr_button).
 * Same offer/MIME machinery as the clipboard, with its own pending/bound
 * slots (primary offers are a different object type, tracked through the
 * shared wp_offer_mimes identity cookie). */

static struct wp_offer_mimes *primsel_mime_slot(wp_platform *pl,
                                                struct zwp_primary_selection_offer_v1 *off) {
    if ((struct zwp_primary_selection_offer_v1 *)pl->primsel_pending_mimes.offer == off)
        return &pl->primsel_pending_mimes;
    if ((struct zwp_primary_selection_offer_v1 *)pl->primsel_offer_mimes.offer == off)
        return &pl->primsel_offer_mimes;
    return nullptr;
}

static void poffer_offer(void *data, struct zwp_primary_selection_offer_v1 *off, const char *mime) {
    wp_platform *pl = data;
    struct wp_offer_mimes *slot = primsel_mime_slot(pl, off);
    if (!slot)
        return;
    if (strcmp(mime, "text/plain;charset=utf-8") == 0)
        slot->text_utf8 = true;
    else if (strcmp(mime, "text/plain") == 0)
        slot->text_plain = true;
}
static const struct zwp_primary_selection_offer_v1_listener primary_offer_listener = {
    .offer = poffer_offer,
};

static void pdev_data_offer(void *data, struct zwp_primary_selection_device_v1 *dev,
                            struct zwp_primary_selection_offer_v1 *offer) {
    wp_platform *pl = data;
    (void)dev;
    /* A previous offer that was announced but never bound is ours to
     * destroy, or it leaks. */
    if (pl->primsel_pending_mimes.offer &&
        (struct zwp_primary_selection_offer_v1 *)pl->primsel_pending_mimes.offer != offer)
        zwp_primary_selection_offer_v1_destroy(
            (struct zwp_primary_selection_offer_v1 *)pl->primsel_pending_mimes.offer);
    pl->primsel_pending_mimes = (struct wp_offer_mimes){.offer = (struct wl_data_offer *)offer};
    zwp_primary_selection_offer_v1_add_listener(offer, &primary_offer_listener, pl);
}
static void pdev_selection(void *data, struct zwp_primary_selection_device_v1 *dev,
                           struct zwp_primary_selection_offer_v1 *offer) {
    wp_platform *pl = data;
    (void)dev;
    if (pl->primsel_offer && pl->primsel_offer != offer)
        zwp_primary_selection_offer_v1_destroy(pl->primsel_offer);
    pl->primsel_offer = offer; /* may be NULL when the selection is cleared */
    pl->primsel_offer_mimes = (struct wp_offer_mimes){.offer = (struct wl_data_offer *)offer};
    if (offer &&
        (struct zwp_primary_selection_offer_v1 *)pl->primsel_pending_mimes.offer == offer) {
        pl->primsel_offer_mimes = pl->primsel_pending_mimes;
        pl->primsel_pending_mimes = (struct wp_offer_mimes){};
    }
}
static const struct zwp_primary_selection_device_v1_listener primary_device_listener = {
    .data_offer = pdev_data_offer,
    .selection = pdev_selection,
};

/* Our outgoing primary selection: serves the same buffer as the clipboard
 * source (wp_clip_set_text mirrors every copy onto both). */
static void psource_send(void *data, struct zwp_primary_selection_source_v1 *src, const char *mime,
                         int32_t fd) {
    wp_platform *pl = data;
    (void)src;
    (void)mime;
    const char *p = pl->copy_buf;
    size_t left = pl->copy_len;
    while (left) {
        ssize_t w = write(fd, p, left);
        if (w <= 0)
            break;
        p += w;
        left -= (size_t)w;
    }
    optics_window_fd_close_record(fd, "fd_primary_write");
}
static void psource_cancelled(void *data, struct zwp_primary_selection_source_v1 *src) {
    wp_platform *pl = data;
    zwp_primary_selection_source_v1_destroy(src);
    if (pl->primsel_source == src)
        pl->primsel_source = nullptr;
}
static const struct zwp_primary_selection_source_v1_listener primary_source_listener = {
    .send = psource_send,
    .cancelled = psource_cancelled,
};

void wp_maybe_create_primsel_device(wp_platform *pl) {
    if (pl->primsel_device || !pl->primsel_mgr || !pl->seat)
        return;
    pl->primsel_device =
        zwp_primary_selection_device_manager_v1_get_device(pl->primsel_mgr, pl->seat);
    zwp_primary_selection_device_v1_add_listener(pl->primsel_device, &primary_device_listener, pl);
}

/* Middle-click paste: request the primary selection's text and deliver it
 * through the same async paste channel as the clipboard (optics_ui_paste routes
 * it to the focused text widget, so a middle click anywhere pastes into
 * whatever holds focus — the standard desktop behaviour). */
void wp_primsel_paste(wp_platform *pl) {
    if (!pl->primsel_offer || !pl->ui)
        return;

    int fds[2];
    if (pipe(fds) != 0)
        return;
    /* Text family only (no uri-list): utf-8 beats legacy text/plain; fall
     * back to utf-8 when the offer predates our MIME tracking. */
    const struct wp_offer_mimes *m = &pl->primsel_offer_mimes;
    const char *mime = ((struct zwp_primary_selection_offer_v1 *)m->offer == pl->primsel_offer &&
                        m->text_plain && !m->text_utf8)
                           ? "text/plain"
                           : "text/plain;charset=utf-8";
    optics_window_fd_open_record(fds[1], "fd_pipe_write");
    optics_window_fd_open_record(fds[0], "fd_pipe_read");
    zwp_primary_selection_offer_v1_receive(pl->primsel_offer, mime, fds[1]);
    optics_window_fd_close_record(fds[1], "fd_pipe_write");
    wl_display_flush(pl->display);

    if (!start_offer_read(pl, fds[0], nullptr, WP_PASTE_TIMEOUT_MS, false))
        optics_window_fd_close_record(fds[0], "fd_pipe_read");
}

/* optics_ui_clipboard.set_text — advertise `utf8` as the system selection, and
 * mirror it onto the primary selection (the X11-style middle-click
 * clipboard) so explicit copies are also middle-pasteable. */
void wp_clip_set_text(const char *utf8, size_t len, void *user) {
    wp_platform *pl = user;
    if (!pl->data_device_mgr || !pl->data_device)
        return;

    char *copy = malloc(len ? len : 1);
    if (!copy)
        return;
    memcpy(copy, utf8, len);
    free(pl->copy_buf);
    pl->copy_buf = copy;
    pl->copy_len = len;

    pl->copy_source = wl_data_device_manager_create_data_source(pl->data_device_mgr);
    wl_data_source_add_listener(pl->copy_source, &data_source_listener, pl);

    bool is_gnome_files = (len >= 5 && strncmp(copy, "copy\n", 5) == 0) ||
                          (len >= 4 && strncmp(copy, "cut\n", 4) == 0);
    bool is_uri_list =
        (len >= 7 && (strncmp(copy, "file://", 7) == 0 || strncmp(copy, "http://", 7) == 0)) ||
        (len >= 8 && strncmp(copy, "https://", 8) == 0);
    if (is_gnome_files) {
        wl_data_source_offer(pl->copy_source, "x-special/gnome-copied-files");
        wl_data_source_offer(pl->copy_source, "text/uri-list");
    } else if (is_uri_list) {
        wl_data_source_offer(pl->copy_source, "text/uri-list");
    }
    wl_data_source_offer(pl->copy_source, "text/plain;charset=utf-8");
    wl_data_source_offer(pl->copy_source, "text/plain");
    wl_data_source_offer(pl->copy_source, "UTF8_STRING");
    wl_data_device_set_selection(pl->data_device, pl->copy_source, pl->last_serial);

    if (pl->primsel_mgr && pl->primsel_device) {
        pl->primsel_source = zwp_primary_selection_device_manager_v1_create_source(pl->primsel_mgr);
        zwp_primary_selection_source_v1_add_listener(pl->primsel_source, &primary_source_listener,
                                                     pl);
        zwp_primary_selection_source_v1_offer(pl->primsel_source, "text/plain;charset=utf-8");
        zwp_primary_selection_source_v1_offer(pl->primsel_source, "text/plain");
        zwp_primary_selection_device_v1_set_selection(pl->primsel_device, pl->primsel_source,
                                                      pl->last_serial);
    }
}

/* optics_ui_clipboard.request_text — the ui contract is asynchronous
 * (cross-platform.md invariant 4): the answer MUST arrive later via
 * optics_ui_paste, never synchronously inside the request. The pipe is drained
 * on a detached helper thread with a hard deadline (start_offer_read), and
 * completion is posted through optics_window_post_to_main_thread — which also makes
 * the self-paste case correct: the main thread keeps dispatching, so our
 * own data source can answer. */
void wp_clip_request_text(void *user) {
    wp_platform *pl = user;
    if (!pl->selection_offer || !pl->ui)
        return;

    int fds[2];
    if (pipe(fds) != 0)
        return;
    /* Negotiate against the offer's advertised types; fall back to the
     * UTF-8 type when the offer predates our MIME tracking (harmless: the
     * source simply refuses a type it does not have). */
    const struct wp_offer_mimes *m = &pl->selection_offer_mimes;
    const char *mime = offer_pick_mime(m);
    if (!mime)
        mime = "text/plain;charset=utf-8";
    optics_window_fd_open_record(fds[1], "fd_pipe_write");
    optics_window_fd_open_record(fds[0], "fd_pipe_read");
    wl_data_offer_receive(pl->selection_offer, mime, fds[1]);
    optics_window_fd_close_record(fds[1], "fd_pipe_write");
    wl_display_flush(pl->display);

    if (!start_offer_read(pl, fds[0], nullptr, WP_PASTE_TIMEOUT_MS, false))
        optics_window_fd_close_record(fds[0], "fd_pipe_read");
}

int wp_start_drag(const char *text, size_t len, uint32_t actions, void *user) {
    (void)user;
    optics_window_dnd_source src = {
        .actions = actions,
        .static_text = text,
        .static_text_len = len,
    };
    return optics_window_dnd_start(&src);
}
