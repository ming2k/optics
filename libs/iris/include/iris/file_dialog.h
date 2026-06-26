/*
 * iris/file_dialog.h — file picker via the host desktop.
 *
 * On Linux, files out to xdg-desktop-portal's FileChooser interface via
 * subprocess (gdbus call). This is the same path modern GTK/Qt take —
 * the user sees their own DE's native picker, and the calling app does
 * not need filesystem UI of its own.
 *
 * Blocking: the call runs the portal's modal loop and returns when the
 * user confirms or cancels. Returns 0 on success, non-zero otherwise.
 */
#ifndef IRIS_FILE_DIALOG_H
#define IRIS_FILE_DIALOG_H

#include <iris/app.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct iris_file_dialog_opts {
    const char *title; /* dialog title (UTF-8, optional)        */
    /* Future: filters, initial_path, save vs open mode, multiple selection. */
} iris_file_dialog_opts;

/* Open a single-file picker. On success, writes a NUL-terminated UTF-8
 * file URI (e.g. "file:///home/user/foo.txt") into out_path and returns 0.
 * Returns non-zero if the user cancels, the portal is unavailable, or
 * out_path is too small (out_cap must be >= the full URI length + 1). */
IRIS_API int iris_pick_file(const iris_file_dialog_opts *opts, char *out_path, size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif /* IRIS_FILE_DIALOG_H */
