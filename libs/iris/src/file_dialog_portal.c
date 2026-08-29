/* file_dialog_portal.c — synchronous wrapper around the asynchronous
 * xdg-desktop-portal FileChooser request.
 *
 * The public Iris API is deliberately blocking, but the portal protocol is
 * not: OpenFile returns a Request object path and the selection arrives in a
 * later Response signal. Keep both operations on one sd-bus connection so the
 * request remains alive, and pump that connection until the user accepts or
 * cancels. No command shell is involved, so arbitrary UTF-8 titles are safe.
 *
 * While the modal picker is open the wait loop ALSO pumps the platform event
 * sources — the Wayland display fd (so xdg_wm_base pings keep being answered
 * and the compositor does not mark the window unresponsive) and the AT-SPI
 * bus fd (so screen readers keep being served). Without that, opening a
 * picker froze the whole app until the user dismissed it. The picker's
 * parent_window is passed as the xdg-foreign handle when the compositor
 * exported one, so the dialog stays modal to our window.
 *
 * Supports open / open-multiple / open-folder / save modes, plus filters
 * (name + glob), an initial URI, and a default save name (SaveFile mode).
 */

#include <errno.h>
#include <iris/file_dialog.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* mode: 0 = OpenFile (single), 1 = OpenFile (multiple), 2 = OpenFile (folder), 3 = SaveFile
 * Defined outside the IRIS_HAVE_PORTAL_WATCH gate: the stub pick_uri below
 * uses the same type. */
typedef enum {
    PICK_OPEN,
    PICK_OPEN_MULTI,
    PICK_FOLDER,
    PICK_SAVE,
} pick_mode;

#ifdef IRIS_HAVE_PORTAL_WATCH
#include "a11y_internal.h"

#include <poll.h>
#include <systemd/sd-bus.h>

#ifdef IRIS_BACKEND_WAYLAND
#include <wayland-client.h>

/* Implemented by app_wayland.c (hidden visibility within libiris). */
struct wl_display *iris_wayland__display(void);
const char *iris_wayland__foreign_handle(void);
#endif

typedef struct iris_picker_response {
    char *out; /* single-URI buffer or NUL-separated multi-URI buffer */
    size_t cap;
    size_t used; /* bytes written (excluding the final NUL) — on overflow,
                    the FULL selection's requirement for retry sizing   */
    int count;   /* URIs received                                       */
    int done;
    int result;
    int overflow; /* set once the cap is hit; stops copying, keeps counting */
} iris_picker_response;

static int append_string_option(sd_bus_message *message, const char *key, const char *value) {
    int rc = sd_bus_message_open_container(message, SD_BUS_TYPE_DICT_ENTRY, "sv");
    if (rc < 0)
        return rc;
    rc = sd_bus_message_append(message, "s", key);
    if (rc >= 0)
        rc = sd_bus_message_open_container(message, SD_BUS_TYPE_VARIANT, "s");
    if (rc >= 0)
        rc = sd_bus_message_append(message, "s", value);
    if (rc >= 0)
        rc = sd_bus_message_close_container(message);
    if (rc >= 0)
        rc = sd_bus_message_close_container(message);
    return rc;
}

static int append_bool_option(sd_bus_message *message, const char *key, int value) {
    int rc = sd_bus_message_open_container(message, SD_BUS_TYPE_DICT_ENTRY, "sv");
    if (rc < 0)
        return rc;
    rc = sd_bus_message_append(message, "s", key);
    if (rc >= 0)
        rc = sd_bus_message_open_container(message, SD_BUS_TYPE_VARIANT, "b");
    if (rc >= 0)
        rc = sd_bus_message_append(message, "b", value);
    if (rc >= 0)
        rc = sd_bus_message_close_container(message);
    if (rc >= 0)
        rc = sd_bus_message_close_container(message);
    return rc;
}

/* Append the "filters" option as a(sa(us)) — see the FileChooser portal spec. */
static int append_filters_option(sd_bus_message *message, const iris_file_filter *filters) {
    if (!filters)
        return 0;
    int rc = sd_bus_message_open_container(message, SD_BUS_TYPE_DICT_ENTRY, "sv");
    if (rc < 0)
        return rc;
    rc = sd_bus_message_append(message, "s", "filters");
    if (rc >= 0)
        rc = sd_bus_message_open_container(message, SD_BUS_TYPE_VARIANT, "a(sa(us))");
    if (rc >= 0)
        rc = sd_bus_message_open_container(message, SD_BUS_TYPE_ARRAY, "(sa(us))");
    while (rc >= 0 && filters->name) {
        rc = sd_bus_message_open_container(message, SD_BUS_TYPE_STRUCT, "sa(us)");
        if (rc < 0)
            break;
        rc = sd_bus_message_append(message, "s", filters->name ? filters->name : "");
        if (rc >= 0)
            rc = sd_bus_message_open_container(message, SD_BUS_TYPE_ARRAY, "(us)");
        /* Parse the semicolon-separated glob list into individual (us) pairs. */
        if (rc >= 0 && filters->pattern) {
            const char *p = filters->pattern;
            while (rc >= 0 && *p) {
                const char *semi = strchr(p, ';');
                size_t len = semi ? (size_t)(semi - p) : strlen(p);
                if (len > 0) {
                    char tmp[256];
                    if (len >= sizeof tmp)
                        len = sizeof tmp - 1;
                    memcpy(tmp, p, len);
                    tmp[len] = '\0';
                    rc = sd_bus_message_open_container(message, SD_BUS_TYPE_STRUCT, "us");
                    if (rc >= 0)
                        rc = sd_bus_message_append(message, "us", 0u, tmp);
                    if (rc >= 0)
                        rc = sd_bus_message_close_container(message);
                }
                if (!semi)
                    break;
                p = semi + 1;
            }
        }
        if (rc >= 0)
            rc = sd_bus_message_close_container(message); /* (us) array */
        if (rc >= 0)
            rc = sd_bus_message_close_container(message); /* struct     */
        ++filters;
    }
    if (rc >= 0)
        rc = sd_bus_message_close_container(message); /* array       */
    if (rc >= 0)
        rc = sd_bus_message_close_container(message); /* variant     */
    if (rc >= 0)
        rc = sd_bus_message_close_container(message); /* dict entry  */
    return rc;
}

static int on_picker_response(sd_bus_message *message, void *userdata, sd_bus_error *error) {
    (void)error;
    iris_picker_response *response = userdata;
    uint32_t portal_result = 2;
    if (sd_bus_message_read(message, "u", &portal_result) < 0) {
        response->done = 1;
        response->result = IRIS_PICK_UNAVAILABLE; /* malformed portal reply */
        return 0;
    }
    if (portal_result != 0) {
        response->done = 1;
        response->result = IRIS_PICK_CANCELLED; /* portal reports non-zero = user dismissed */
        return 0;
    }

    int rc = sd_bus_message_enter_container(message, SD_BUS_TYPE_ARRAY, "{sv}");
    if (rc < 0) {
        response->done = 1;
        response->result = IRIS_PICK_UNAVAILABLE; /* malformed portal reply */
        return 0;
    }
    while ((rc = sd_bus_message_enter_container(message, SD_BUS_TYPE_DICT_ENTRY, "sv")) > 0) {
        const char *key = NULL;
        if (sd_bus_message_read(message, "s", &key) < 0)
            break;
        if (key && strcmp(key, "uris") == 0) {
            if (sd_bus_message_enter_container(message, SD_BUS_TYPE_VARIANT, "as") >= 0 &&
                sd_bus_message_enter_container(message, SD_BUS_TYPE_ARRAY, "s") >= 0) {
                const char *uri = NULL;
                while (sd_bus_message_read(message, "s", &uri) > 0 && uri) {
                    size_t len = strlen(uri);
                    /* +1 for the NUL terminator (single) or separator (multi). */
                    if (response->used + len + 1 > response->cap) {
                        /* Contract (file_dialog.h): truncation is a visible,
                         * uniform failure across backends — never a silent
                         * prefix, never a disguised cancel. Keep counting so
                         * *out_bytes_used can report the FULL requirement
                         * and the caller can size the retry exactly. */
                        response->result = IRIS_PICK_TRUNCATED;
                        response->overflow = 1;
                    }
                    if (!response->overflow) {
                        memcpy(response->out + response->used, uri, len + 1);
                        response->used += len + 1;
                        response->count++;
                    } else {
                        /* Still accounting for the retry-size report. */
                        response->used += len + 1;
                        response->count++;
                    }
                    if (!response->overflow && response->result != 0)
                        response->result = 0;
                    /* Continue for multi-select; single-select stops at one. */
                }
                (void)sd_bus_message_exit_container(message);
                (void)sd_bus_message_exit_container(message);
            }
        } else {
            (void)sd_bus_message_skip(message, "v");
        }
        (void)sd_bus_message_exit_container(message);
    }
    (void)sd_bus_message_exit_container(message);
    response->done = 1;
    return 0;
}

static int pick_uri(const iris_file_dialog_opts *opts, pick_mode mode, const char *default_name,
                    char *out_path, size_t out_cap, size_t *out_used, int *out_count) {
    if (!out_path || out_cap == 0)
        return IRIS_PICK_UNAVAILABLE; /* invalid arguments: no dialog can run */
    out_path[0] = '\0';

    const char *title;
    const char *method;
    switch (mode) {
    case PICK_OPEN_MULTI:
        title = (opts && opts->title) ? opts->title : "Open Files";
        method = "OpenFile";
        break;
    case PICK_FOLDER:
        title = (opts && opts->title) ? opts->title : "Open Folder";
        method = "OpenFile";
        break;
    case PICK_SAVE:
        title = (opts && opts->title) ? opts->title : "Save As";
        method = "SaveFile";
        break;
    case PICK_OPEN:
    default:
        title = (opts && opts->title) ? opts->title : "Open File";
        method = "OpenFile";
        break;
    }

    sd_bus *bus = NULL;
    sd_bus_message *call = NULL;
    sd_bus_message *reply = NULL;
    sd_bus_slot *slot = NULL;
    sd_bus_error error = SD_BUS_ERROR_NULL;
    int result = IRIS_PICK_UNAVAILABLE; /* bus/portal setup failures */

    int rc = sd_bus_open_user(&bus);
    if (rc < 0)
        goto cleanup;
    rc = sd_bus_message_new_method_call(bus, &call, "org.freedesktop.portal.Desktop",
                                        "/org/freedesktop/portal/desktop",
                                        "org.freedesktop.portal.FileChooser", method);
    if (rc < 0)
        goto cleanup;
    /* parent_window: "wayland:<xdg-foreign handle>" when the compositor
     * exported one (app_wayland.c); empty means "no parent" — the picker is
     * then not modal to us, which some portals render as a free window. */
    char parent[128] = "";
#ifdef IRIS_BACKEND_WAYLAND
    const char *foreign = iris_wayland__foreign_handle();
    if (foreign)
        (void)snprintf(parent, sizeof parent, "wayland:%s", foreign);
#endif
    rc = sd_bus_message_append(call, "ss", parent, title);
    if (rc < 0)
        goto cleanup;
    rc = sd_bus_message_open_container(call, SD_BUS_TYPE_ARRAY, "{sv}");
    if (rc < 0)
        goto cleanup;

    struct timespec now = {0};
    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    char token[64];
    (void)snprintf(token, sizeof token, "iris_%ld_%ld", (long)now.tv_sec, now.tv_nsec);
    rc = append_string_option(call, "handle_token", token);
    if (rc >= 0 && mode == PICK_FOLDER)
        rc = append_bool_option(call, "directory", 1);
    if (rc >= 0 && mode == PICK_OPEN_MULTI)
        rc = append_bool_option(call, "multiple", 1);
    if (rc >= 0 && opts && opts->directory_only)
        rc = append_bool_option(call, "directory", 1);
    if (rc >= 0 && opts && opts->multiple_selection && mode == PICK_OPEN)
        rc = append_bool_option(call, "multiple", 1);
    if (rc >= 0 && opts && opts->filters)
        rc = append_filters_option(call, opts->filters);
    if (rc >= 0 && opts && opts->initial_uri)
        rc = append_string_option(call, "current_folder", opts->initial_uri);
    if (rc >= 0 && mode == PICK_SAVE && default_name)
        rc = append_string_option(call, "current_name", default_name);
    if (rc >= 0)
        rc = sd_bus_message_close_container(call);
    if (rc < 0)
        goto cleanup;

    iris_picker_response response = {
        .out = out_path,
        .cap = out_cap,
        .used = 0,
        .count = 0,
        .done = 0,
        .result = 5,
    };
    /* Register before sending the request. A fast portal implementation is
     * allowed to emit Response immediately, before the method reply reaches
     * us. This dedicated bus connection has only this one picker request, so
     * matching any Request.Response path is both safe and race-free. */
    rc = sd_bus_match_signal(bus, &slot, "org.freedesktop.portal.Desktop", NULL,
                             "org.freedesktop.portal.Request", "Response", on_picker_response,
                             &response);
    if (rc < 0)
        goto cleanup;

    rc = sd_bus_call(bus, call, 0, &error, &reply);
    if (rc < 0)
        goto cleanup;
    const char *request_path = NULL;
    rc = sd_bus_message_read(reply, "o", &request_path);
    if (rc < 0 || !request_path)
        goto cleanup;

    /* Wait for the Response signal. The wait pumps THREE event sources so
     * the app stays alive while the modal picker is open:
     *   - the portal bus fd (the answer we are waiting for),
     *   - the Wayland display fd — the compositor pings xdg_wm_base while a
     *     modal dialog is up; not answering marks the window unresponsive
     *     (the audit's "file dialog freezes the app" defect),
     *   - the AT-SPI bus fd — screen readers keep being served.
     * The 250 ms cap only keeps the sd-bus poll masks fresh. */
    while (!response.done) {
        do {
            rc = sd_bus_process(bus, NULL);
        } while (rc > 0 && !response.done);
        if (rc < 0)
            goto cleanup;
        if (response.done)
            break;

        struct pollfd pfds[3];
        int n = 0;
        int wl_idx = -1, a11y_idx = -1;
        int bus_fd = sd_bus_get_fd(bus);
        if (bus_fd < 0)
            goto cleanup;
        pfds[n++] = (struct pollfd){.fd = bus_fd, .events = (short)sd_bus_get_events(bus)};

#ifdef IRIS_BACKEND_WAYLAND
        struct wl_display *wl = iris_wayland__display();
        if (wl) {
            /* Flush + drain anything already queued, then watch for more. */
            wl_display_flush(wl);
            while (wl_display_dispatch_pending(wl) > 0) {
            }
            wl_idx = n;
            pfds[n++] = (struct pollfd){.fd = wl_display_get_fd(wl), .events = POLLIN};
        }
#endif
        int afd = iris_a11y__fd();
        short aev = iris_a11y__poll_events();
        if (afd >= 0 && aev != 0) {
            a11y_idx = n;
            pfds[n++] = (struct pollfd){.fd = afd, .events = aev};
        }

        if (poll(pfds, n, 250) < 0) {
            if (errno == EINTR)
                continue;
            goto cleanup;
        }

#ifdef IRIS_BACKEND_WAYLAND
        if (wl_idx >= 0 && (pfds[wl_idx].revents & POLLIN)) {
            /* Read + dispatch so compositor pings get their pong. Single-
             * threaded here (the dialog runs on the event-loop thread), so
             * the prepare_read dance is safe. */
            while (wl_display_prepare_read(wl) != 0)
                wl_display_dispatch_pending(wl);
            wl_display_flush(wl);
            if (wl_display_read_events(wl) == 0)
                wl_display_dispatch_pending(wl);
            else
                wl_display_cancel_read(wl);
        }
#endif
        if (a11y_idx >= 0 && pfds[a11y_idx].revents)
            iris_a11y__pump();
    }
    result = response.result;
    if (out_used)
        *out_used = response.used;
    if (out_count)
        *out_count = response.count;

cleanup:
    sd_bus_error_free(&error);
    sd_bus_slot_unref(slot);
    sd_bus_message_unref(reply);
    sd_bus_message_unref(call);
    sd_bus_unref(bus);
    return result;
}

#else

static int pick_uri(const iris_file_dialog_opts *opts, pick_mode mode, const char *default_name,
                    char *out_path, size_t out_cap, size_t *out_used, int *out_count) {
    (void)opts;
    (void)mode;
    (void)default_name;
    (void)out_used;
    (void)out_count;
    if (out_path && out_cap > 0)
        out_path[0] = '\0';
    return 6;
}

#endif

/* Uniform wrapper policy (file_dialog.h contract): a single-file picker
 * never widens into a multi-select because the caller happened to leave
 * multiple_selection set — the option belongs to iris_pick_files. */
static iris_file_dialog_opts irisi_single_opts(const iris_file_dialog_opts *opts) {
    iris_file_dialog_opts o = opts ? *opts : (iris_file_dialog_opts){0};
    o.multiple_selection = false;
    return o;
}

IRIS_API int iris_pick_file(const iris_file_dialog_opts *opts, char *out_path, size_t out_cap) {
    iris_file_dialog_opts o = irisi_single_opts(opts);
    return pick_uri(&o, PICK_OPEN, NULL, out_path, out_cap, NULL, NULL);
}

IRIS_API int iris_pick_folder(const iris_file_dialog_opts *opts, char *out_path, size_t out_cap) {
    iris_file_dialog_opts o = irisi_single_opts(opts);
    return pick_uri(&o, PICK_FOLDER, NULL, out_path, out_cap, NULL, NULL);
}

IRIS_API int iris_pick_save_path(const iris_file_dialog_opts *opts, const char *default_name,
                                 char *out_path, size_t out_cap) {
    iris_file_dialog_opts o = irisi_single_opts(opts);
    return pick_uri(&o, PICK_SAVE, default_name, out_path, out_cap, NULL, NULL);
}

/* Success path returns the count; every documented failure propagates its
 * code (IRIS_PICK_*). The old wrapper folded every non-zero rc into 0,
 * which made buffer-overflow indistinguishable from a user cancel. */
IRIS_API int iris_pick_files(const iris_file_dialog_opts *opts, char *out_paths, size_t out_cap,
                             size_t *out_bytes_used) {
    int count = 0;
    int rc = pick_uri(opts, PICK_OPEN_MULTI, NULL, out_paths, out_cap, out_bytes_used, &count);
    if (rc != 0)
        return rc;
    return count;
}
