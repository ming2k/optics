/* file_dialog_portal.c — synchronous wrapper around the asynchronous
 * xdg-desktop-portal FileChooser request.
 *
 * The public Iris API is deliberately blocking, but the portal protocol is
 * not: OpenFile returns a Request object path and the selection arrives in a
 * later Response signal. Keep both operations on one sd-bus connection so the
 * request remains alive, and pump that connection until the user accepts or
 * cancels. No command shell is involved, so arbitrary UTF-8 titles are safe.
 */

#include <iris/file_dialog.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef IRIS_HAVE_PORTAL_WATCH
#include <systemd/sd-bus.h>

typedef struct iris_picker_response {
    char *out;
    size_t cap;
    int done;
    int result;
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

static int on_picker_response(sd_bus_message *message, void *userdata, sd_bus_error *error) {
    (void)error;
    iris_picker_response *response = userdata;
    uint32_t portal_result = 2;
    if (sd_bus_message_read(message, "u", &portal_result) < 0) {
        response->done = 1;
        response->result = 7;
        return 0;
    }
    if (portal_result != 0) {
        response->done = 1;
        response->result = 4;
        return 0;
    }

    int rc = sd_bus_message_enter_container(message, SD_BUS_TYPE_ARRAY, "{sv}");
    if (rc < 0) {
        response->done = 1;
        response->result = 7;
        return 0;
    }
    while ((rc = sd_bus_message_enter_container(message, SD_BUS_TYPE_DICT_ENTRY, "sv")) > 0) {
        const char *key = NULL;
        if (sd_bus_message_read(message, "s", &key) < 0)
            break;
        if (key && strcmp(key, "uris") == 0) {
            const char *uri = NULL;
            if (sd_bus_message_enter_container(message, SD_BUS_TYPE_VARIANT, "as") >= 0 &&
                sd_bus_message_enter_container(message, SD_BUS_TYPE_ARRAY, "s") >= 0 &&
                sd_bus_message_read(message, "s", &uri) > 0 && uri) {
                size_t length = strlen(uri);
                if (length + 1 <= response->cap) {
                    memcpy(response->out, uri, length + 1);
                    response->result = 0;
                } else {
                    response->result = 8;
                }
            }
            (void)sd_bus_message_exit_container(message);
            (void)sd_bus_message_exit_container(message);
        } else {
            (void)sd_bus_message_skip(message, "v");
        }
        (void)sd_bus_message_exit_container(message);
    }
    (void)sd_bus_message_exit_container(message);
    response->done = 1;
    return 0;
}

static int pick_uri(const iris_file_dialog_opts *opts, int directory, char *out_path,
                    size_t out_cap) {
    if (!out_path || out_cap == 0)
        return 1;
    out_path[0] = '\0';

    const char *title = (opts && opts->title) ? opts->title :
                                                   (directory ? "Open Folder" : "Open File");
    sd_bus *bus = NULL;
    sd_bus_message *call = NULL;
    sd_bus_message *reply = NULL;
    sd_bus_slot *slot = NULL;
    sd_bus_error error = SD_BUS_ERROR_NULL;
    int result = 6;

    int rc = sd_bus_open_user(&bus);
    if (rc < 0)
        goto cleanup;
    rc = sd_bus_message_new_method_call(bus, &call, "org.freedesktop.portal.Desktop",
                                        "/org/freedesktop/portal/desktop",
                                        "org.freedesktop.portal.FileChooser", "OpenFile");
    if (rc < 0)
        goto cleanup;
    rc = sd_bus_message_append(call, "ss", "", title);
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
    if (rc >= 0 && directory)
        rc = append_bool_option(call, "directory", 1);
    if (rc >= 0)
        rc = sd_bus_message_close_container(call);
    if (rc < 0)
        goto cleanup;

    iris_picker_response response = {
        .out = out_path,
        .cap = out_cap,
        .done = 0,
        .result = 5,
    };
    /* Register before sending OpenFile. A fast portal implementation is
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

    while (!response.done) {
        do {
            rc = sd_bus_process(bus, NULL);
        } while (rc > 0 && !response.done);
        if (rc < 0)
            goto cleanup;
        if (!response.done && sd_bus_wait(bus, UINT64_MAX) < 0)
            goto cleanup;
    }
    result = response.result;

cleanup:
    sd_bus_error_free(&error);
    sd_bus_slot_unref(slot);
    sd_bus_message_unref(reply);
    sd_bus_message_unref(call);
    sd_bus_unref(bus);
    return result;
}

#else

static int pick_uri(const iris_file_dialog_opts *opts, int directory, char *out_path,
                    size_t out_cap) {
    (void)opts;
    (void)directory;
    if (out_path && out_cap > 0)
        out_path[0] = '\0';
    return 6;
}

#endif

IRIS_API int iris_pick_file(const iris_file_dialog_opts *opts, char *out_path, size_t out_cap) {
    return pick_uri(opts, 0, out_path, out_cap);
}

IRIS_API int iris_pick_folder(const iris_file_dialog_opts *opts, char *out_path, size_t out_cap) {
    return pick_uri(opts, 1, out_path, out_cap);
}
