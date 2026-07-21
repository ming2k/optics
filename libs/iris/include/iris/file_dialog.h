/*
 * iris/file_dialog.h — file picker via the host desktop.
 *
 * On Linux, talks directly to xdg-desktop-portal's FileChooser interface
 * over the user D-Bus. This is the same path modern GTK/Qt take — the user
 * sees their own DE's native picker, and the calling app does not need
 * filesystem UI of its own.
 *
 * Blocking: the call runs the portal's modal loop and returns when the
 * user confirms or cancels. Returns 0 on success, non-zero otherwise.
 */
#ifndef IRIS_FILE_DIALOG_H
#define IRIS_FILE_DIALOG_H

#include <iris/app.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A name + glob pair such as {"Text", "*.txt"}. `pattern` may be a single
 * glob ("*.txt") or a semicolon-separated list ("*.txt;*.md"). A NULL or
 * empty pattern means "any file". */
typedef struct iris_file_filter {
    const char *name;    /* human label, UTF-8 (optional)            */
    const char *pattern; /* glob expression, UTF-8 (optional)        */
} iris_file_filter;

typedef struct iris_file_dialog_opts {
    const char *title;       /* dialog title (UTF-8, optional)        */
    const char *initial_uri; /* file:// URI to start at (optional)    */
    const iris_file_filter *filters; /* NULL-terminated array (opt)   */
    bool multiple_selection; /* allow more than one (open mode only)  */
    bool directory_only;     /* restrict to directories               */
} iris_file_dialog_opts;

/* Open a single-file picker. On success, writes a NUL-terminated UTF-8
 * file URI (e.g. "file:///home/user/foo.txt") into out_path and returns 0.
 * Returns non-zero if the user cancels, the portal is unavailable, or
 * out_path is too small (out_cap must be >= the full URI length + 1). */
IRIS_API int iris_pick_file(const iris_file_dialog_opts *opts, char *out_path, size_t out_cap);

/* Open a single-folder picker. The return convention and URI encoding match
 * iris_pick_file. */
IRIS_API int iris_pick_folder(const iris_file_dialog_opts *opts, char *out_path, size_t out_cap);

/* Save dialog: asks the user for a destination path. `default_name` is the
 * suggested filename (UTF-8, optional); on success a file:// URI is written
 * to out_path. The file is not created — opening it for write is the host's
 * responsibility. */
IRIS_API int iris_pick_save_path(const iris_file_dialog_opts *opts, const char *default_name,
                                 char *out_path, size_t out_cap);

/* Multi-selection picker. On success, writes NUL-separated UTF-8 file URIs
 * into out_paths and returns the number of URIs (>= 1); 0 on cancel or
 * failure. *out_bytes_used (optional) receives the total bytes written
 * (excluding the trailing NUL terminator). */
IRIS_API int iris_pick_files(const iris_file_dialog_opts *opts, char *out_paths, size_t out_cap,
                             size_t *out_bytes_used);

#ifdef __cplusplus
}
#endif

#endif /* IRIS_FILE_DIALOG_H */
