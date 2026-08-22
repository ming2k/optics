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
    const char *title;               /* dialog title (UTF-8, optional)        */
    const char *initial_uri;         /* file:// URI to start at (optional)    */
    const iris_file_filter *filters; /* NULL-terminated array (opt)   */
    bool multiple_selection;         /* allow more than one (open mode only)  */
    bool directory_only;             /* restrict to directories               */
} iris_file_dialog_opts;

/* Result codes shared by every picker. Non-zero means "no selection".
 * The specific negative values let a host distinguish "the user changed
 * their mind" from "your buffer was too small" — which previously
 * behaved differently per backend (portal reported a cancel, Win32
 * silently truncated, Cocoa failed) with none of it documented. */
#define IRIS_PICK_CANCELLED (-1)    /* user dismissed the dialog              */
#define IRIS_PICK_UNAVAILABLE (-2)  /* no dialog service on this platform     */
#define IRIS_PICK_TRUNCATED (-3)    /* out buffer too small: retry with more  */

/* Open a single-file picker. On success, writes a NUL-terminated UTF-8
 * file URI (e.g. "file:///home/user/foo.txt") into out_path and returns 0.
 * `multiple_selection` is ignored here (it belongs to iris_pick_files);
 * passing it does not widen this dialog on any backend. Returns
 * IRIS_PICK_CANCELLED / IRIS_PICK_UNAVAILABLE / IRIS_PICK_TRUNCATED. */
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
 * into out_paths (the list is terminated by an empty URI: one extra NUL
 * after the last entry) and returns the number of URIs (>= 1).
 *
 * Buffer-too-small is IRIS_PICK_TRUNCATED on EVERY backend: the result is
 * never a silent prefix and never indistinguishable from a cancel. The
 * complete selection is not recoverable after truncation — reopen the
 * dialog with a larger buffer. *out_bytes_used (optional) receives the
 * total bytes written excluding the trailing list terminator NUL; on
 * IRIS_PICK_TRUNCATED it receives the number of bytes the FULL selection
 * would have required, so the retry size is directly computable. */
IRIS_API int iris_pick_files(const iris_file_dialog_opts *opts, char *out_paths, size_t out_cap,
                             size_t *out_bytes_used);

/* Convert a `file://` URI (as returned by the pickers above) into a local
 * filesystem path, percent-decoding %XX escapes. Returns 0 on success.
 * Returns -1 for invalid arguments or a URI that is not `file://`,
 * -2 for a non-empty authority component (`file://host/...` is a remote
 * resource, not a local path), -3 for a malformed escape (a '%' not
 * followed by two hex digits), and -4 when out_cap is too small for the
 * decoded path. Non-ASCII bytes pass through verbatim: the portal emits
 * percent-encoded ASCII, and a host supplying raw UTF-8 gets it back. */
IRIS_API int iris_file_uri_to_path(const char *uri, char *out_path, size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif /* IRIS_FILE_DIALOG_H */
