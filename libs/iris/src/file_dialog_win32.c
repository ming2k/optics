/* file_dialog_win32.c — file picker via the Windows common item dialogs.
 *
 * IFileOpenDialog / IFileSaveDialog (Vista+) driven through the C COM
 * vtables (->lpVtbl->), matching the blocking contract of file_dialog.h:
 * Show() runs the modal loop and returns when the user confirms or cancels.
 * Results are converted to file:// URIs (UTF-16 -> UTF-8, backslashes to
 * forward slashes, percent-encoding) — the same shape the portal backend
 * returns on Linux.
 *
 * The dialogs are owned by the active iris window when one exists
 * (iris_win32__dialog_owner, provided by app_win32.c) so they stay modal to
 * the app; without a running app they are top-level.
 *
 * NOT YET VERIFIED ON A REAL WINDOWS MACHINE (zig compile-check only). The
 * URI encoding/decoding round-trip with non-ASCII paths and the multi-select
 * IShellItemArray walk are the parts most worth an on-hardware pass.
 */

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

/* meson supplies -DIRIS_BUILDING=1; define it so single-file compile checks
 * (tools/zig-win32-check.sh) don't see dllimport on definitions. */
#ifndef IRIS_BUILDING
#define IRIS_BUILDING 1
#endif

#include <windows.h>

#include <objbase.h>
#include <shobjidl.h>

#include <iris/file_dialog.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Provided by app_win32.c: the active app window, or NULL when no app runs. */
HWND iris_win32__dialog_owner(void);

/* ------------------------------------------------------------------ */
/*  String / URI helpers                                              */
/* ------------------------------------------------------------------ */

/* UTF-8 -> freshly allocated NUL-terminated WCHAR (NULL on failure). */
static WCHAR *wide_from_utf8(const char *utf8) {
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (n <= 0)
        return NULL;
    WCHAR *w = malloc((size_t)n * sizeof *w);
    if (!w)
        return NULL;
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, n);
    return w;
}

/* WCHAR -> freshly allocated NUL-terminated UTF-8 (NULL on failure). */
static char *utf8_from_wide(const WCHAR *w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0)
        return NULL;
    char *u8 = malloc((size_t)n);
    if (!u8)
        return NULL;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, u8, n, NULL, NULL);
    return u8;
}

static bool uri_byte_unreserved(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
           c == '.' || c == '_' || c == '~';
}

/* Native path (C:\dir\file.txt) -> freshly allocated file URI
 * (file:///C:/dir/file.txt). Backslashes become forward slashes; everything
 * outside the RFC 3986 unreserved set (plus the structural '/' and the drive
 * ':') is percent-encoded, so spaces and non-ASCII UTF-8 bytes survive the
 * trip. */
static char *path_to_file_uri(const WCHAR *wpath) {
    char *u8 = utf8_from_wide(wpath);
    if (!u8)
        return NULL;
    size_t len = strlen(u8);
    char *uri = malloc(strlen("file:///") + len * 3 + 1); /* worst case all %XX */
    if (!uri) {
        free(u8);
        return NULL;
    }
    char *p = uri;
    memcpy(p, "file:///", strlen("file:///"));
    p += strlen("file:///");
    static const char hexdig[] = "0123456789ABCDEF";
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)u8[i];
        if (c == '\\') {
            *p++ = '/';
        } else if (uri_byte_unreserved(c) || c == '/' || c == ':') {
            *p++ = (char)c;
        } else {
            p[0] = '%';
            p[1] = hexdig[c >> 4];
            p[2] = hexdig[c & 0xF];
            p += 3;
        }
    }
    *p = '\0';
    free(u8);
    return uri;
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* file:// URI -> freshly allocated native path (WCHAR), or NULL when the
 * URI is not a plain local file URI. Percent-decoded, forward slashes become
 * backslashes, and the "/C:/..." spelling produced by path_to_file_uri loses
 * its leading slash. */
static WCHAR *file_uri_to_path(const char *uri) {
    if (!uri || strncmp(uri, "file://", 7) != 0)
        return NULL;
    const char *p = uri + 7;
    if (strncmp(p, "localhost/", 10) == 0)
        p += 9; /* keep the slash that follows the authority */

    size_t len = strlen(p);
    char *tmp = malloc(len + 1);
    if (!tmp)
        return NULL;
    char *o = tmp;
    while (*p) {
        if (p[0] == '%' && p[1] && p[2] && hex_value(p[1]) >= 0 && hex_value(p[2]) >= 0) {
            *o++ = (char)(hex_value(p[1]) * 16 + hex_value(p[2]));
            p += 3;
        } else if (*p == '/') {
            *o++ = '\\';
            p++;
        } else {
            *o++ = *p++;
        }
    }
    *o = '\0';

    /* "\C:\dir" (from "file:///C:/dir") -> "C:\dir". */
    const char *path = tmp;
    if (tmp[0] == '\\' && isalpha((unsigned char)tmp[1]) && tmp[2] == ':')
        path = tmp + 1;
    WCHAR *w = wide_from_utf8(path);
    free(tmp);
    return w;
}

/* Write one URI into the output buffer. Returns 0 on success, non-zero when
 * out_cap is too small (the caller then reports failure, like the portal
 * backend's result code 8). */
static int emit_single_uri(const WCHAR *wpath, char *out_path, size_t out_cap) {
    char *uri = path_to_file_uri(wpath);
    if (!uri)
        return 1;
    size_t len = strlen(uri);
    if (len + 1 > out_cap) {
        free(uri);
        return 1;
    }
    memcpy(out_path, uri, len + 1);
    free(uri);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Dialog option plumbing                                             */
/* ------------------------------------------------------------------ */

/* Convert the public filter list into a COMDLG_FILTERSPEC array. The list
 * is terminated by an entry with name == NULL (same convention the portal
 * backend uses). An empty/NULL pattern means "any file" (*.*). */
static HRESULT set_filters(IFileDialog *dlg, const iris_file_filter *filters) {
    if (!filters || !filters->name)
        return S_OK; /* no filters: the dialog defaults to *.* */
    UINT count = 0;
    while (filters[count].name)
        count++;

    COMDLG_FILTERSPEC *specs = calloc(count, sizeof *specs);
    if (!specs)
        return E_OUTOFMEMORY;
    HRESULT hr = S_OK;
    for (UINT i = 0; i < count; i++) {
        const char *name = filters[i].name;
        const char *pattern =
            (filters[i].pattern && filters[i].pattern[0]) ? filters[i].pattern : "*.*";
        /* The Win32 spec string accepts the portal's "*.txt;*.md" list as-is. */
        specs[i].pszName = wide_from_utf8(name);
        specs[i].pszSpec = wide_from_utf8(pattern);
        if (!specs[i].pszName || !specs[i].pszSpec) {
            hr = E_OUTOFMEMORY;
            break;
        }
    }
    if (SUCCEEDED(hr))
        hr = dlg->lpVtbl->SetFileTypes(dlg, count, specs);
    for (UINT i = 0; i < count; i++) {
        free((void *)specs[i].pszName);
        free((void *)specs[i].pszSpec);
    }
    free(specs);
    return hr;
}

/* SetTitle + SetDefaultFolder from the public options. Both are optional;
 * an initial URI that doesn't parse or doesn't exist is simply ignored. */
static void apply_common_options(IFileDialog *dlg, const iris_file_dialog_opts *opts) {
    if (!opts)
        return;
    if (opts->title) {
        WCHAR *title = wide_from_utf8(opts->title);
        if (title) {
            (void)dlg->lpVtbl->SetTitle(dlg, title);
            free(title);
        }
    }
    if (opts->initial_uri) {
        WCHAR *path = file_uri_to_path(opts->initial_uri);
        if (path) {
            IShellItem *folder = NULL;
            if (SHCreateItemFromParsingName(path, NULL, &IID_IShellItem, (void **)&folder) ==
                S_OK) {
                (void)dlg->lpVtbl->SetDefaultFolder(dlg, folder);
                folder->lpVtbl->Release(folder);
            }
            free(path);
        }
    }
}

/* One shell item -> its file-system path string (CoTaskMemFree'd by the
 * caller). FOS_FORCEFILESYSTEM is set on every dialog, so SIGDN_FILESYSPATH
 * is always available. */
static WCHAR *shell_item_path(IShellItem *item) {
    LPWSTR path = NULL;
    if (item->lpVtbl->GetDisplayName(item, SIGDN_FILESYSPATH, &path) != S_OK || !path)
        return NULL;
    /* CoTaskMemFree happens in the caller-facing wrapper below. */
    return path;
}

/* ------------------------------------------------------------------ */
/*  Public entry points                                                */
/* ------------------------------------------------------------------ */

/* Open-mode implementation shared by pick_file / pick_folder / pick_files.
 * Returns the number of URIs written (>= 1), 0 on cancel/failure. On
 * single-selection success out_path holds one NUL-terminated URI; on
 * multi-selection out_paths holds a NUL-separated list and *out_bytes_used
 * (optional) the total bytes written including each URI's NUL separator —
 * matching the portal backend's accounting. */
static int pick_open(const iris_file_dialog_opts *opts, bool multiple, bool folder, char *out_paths,
                     size_t out_cap, size_t *out_bytes_used) {
    if (!out_paths || out_cap == 0)
        return 0;
    out_paths[0] = '\0';
    if (out_bytes_used)
        *out_bytes_used = 0;

    /* Every successful CoInitializeEx (S_OK or S_FALSE) pairs with one
     * CoUninitialize; RPC_E_CHANGED_MODE (someone else required MTA) means
     * we must not. */
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr))
        return 0;

    int count = 0;
    size_t used = 0;
    IFileOpenDialog *dlg = NULL;
    hr = CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_ALL, &IID_IFileOpenDialog,
                          (void **)&dlg);
    if (FAILED(hr) || !dlg)
        goto done;

    DWORD opts_flags = 0;
    if (dlg->lpVtbl->GetOptions(dlg, &opts_flags) != S_OK)
        goto done;
    opts_flags |= FOS_FORCEFILESYSTEM;
    if (multiple)
        opts_flags |= FOS_ALLOWMULTISELECT;
    if (folder || (opts && opts->directory_only))
        opts_flags |= FOS_PICKFOLDERS;
    if (dlg->lpVtbl->SetOptions(dlg, opts_flags) != S_OK)
        goto done;

    apply_common_options((IFileDialog *)dlg, opts);
    if (set_filters((IFileDialog *)dlg, opts ? opts->filters : NULL) != S_OK)
        goto done;

    hr = dlg->lpVtbl->Show(dlg, iris_win32__dialog_owner());
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED))
        goto done; /* user cancel: 0 URIs, not an error to report */
    if (FAILED(hr))
        goto done;

    if (multiple) {
        IShellItemArray *arr = NULL;
        if (dlg->lpVtbl->GetResults(dlg, &arr) != S_OK || !arr)
            goto done;
        DWORD n = 0;
        (void)arr->lpVtbl->GetCount(arr, &n);
        for (DWORD i = 0; i < n; i++) {
            IShellItem *item = NULL;
            if (arr->lpVtbl->GetItemAt(arr, i, &item) != S_OK || !item)
                continue;
            LPWSTR path = shell_item_path(item);
            if (path) {
                char *uri = path_to_file_uri(path);
                CoTaskMemFree(path);
                if (uri) {
                    size_t len = strlen(uri);
                    /* Buffer-full stops the walk: the result is the
                     * selection's prefix, still a valid NUL-separated list. */
                    if (used + len + 1 > out_cap) {
                        free(uri);
                        item->lpVtbl->Release(item);
                        break;
                    }
                    memcpy(out_paths + used, uri, len + 1);
                    used += len + 1;
                    count++;
                    free(uri);
                }
            }
            item->lpVtbl->Release(item);
        }
        arr->lpVtbl->Release(arr);
    } else {
        IShellItem *item = NULL;
        if (dlg->lpVtbl->GetResult(dlg, &item) == S_OK && item) {
            LPWSTR path = shell_item_path(item);
            if (path) {
                if (emit_single_uri(path, out_paths, out_cap) == 0) {
                    count = 1;
                    used = strlen(out_paths) + 1;
                }
                CoTaskMemFree(path);
            }
            item->lpVtbl->Release(item);
        }
    }

done:
    if (dlg)
        dlg->lpVtbl->Release(dlg);
    CoUninitialize();
    if (out_bytes_used)
        *out_bytes_used = used;
    return count;
}

IRIS_API int iris_pick_file(const iris_file_dialog_opts *opts, char *out_path, size_t out_cap) {
    /* opts->multiple_selection is honoured for parity with the portal
     * backend (the dialog allows it; the first result wins). */
    bool multiple = opts && opts->multiple_selection;
    return pick_open(opts, multiple, false, out_path, out_cap, NULL) == 1 ? 0 : 1;
}

IRIS_API int iris_pick_folder(const iris_file_dialog_opts *opts, char *out_path, size_t out_cap) {
    return pick_open(opts, false, true, out_path, out_cap, NULL) == 1 ? 0 : 1;
}

IRIS_API int iris_pick_save_path(const iris_file_dialog_opts *opts, const char *default_name,
                                 char *out_path, size_t out_cap) {
    if (!out_path || out_cap == 0)
        return 1;
    out_path[0] = '\0';

    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr))
        return 1;

    int result = 1;
    IFileSaveDialog *dlg = NULL;
    hr = CoCreateInstance(&CLSID_FileSaveDialog, NULL, CLSCTX_ALL, &IID_IFileSaveDialog,
                          (void **)&dlg);
    if (FAILED(hr) || !dlg)
        goto done;

    DWORD opts_flags = 0;
    if (dlg->lpVtbl->GetOptions(dlg, &opts_flags) != S_OK)
        goto done;
    /* FOS_OVERWRITEPROMPT is the Explorer default and stays on. */
    if (dlg->lpVtbl->SetOptions(dlg, opts_flags | FOS_FORCEFILESYSTEM) != S_OK)
        goto done;

    apply_common_options((IFileDialog *)dlg, opts);
    if (set_filters((IFileDialog *)dlg, opts ? opts->filters : NULL) != S_OK)
        goto done;
    if (default_name) {
        WCHAR *name = wide_from_utf8(default_name);
        if (name) {
            (void)dlg->lpVtbl->SetFileName(dlg, name);
            free(name);
        }
    }

    hr = dlg->lpVtbl->Show(dlg, iris_win32__dialog_owner());
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED))
        goto done;
    if (FAILED(hr))
        goto done;

    IShellItem *item = NULL;
    if (dlg->lpVtbl->GetResult(dlg, &item) == S_OK && item) {
        LPWSTR path = shell_item_path(item);
        if (path) {
            /* The dialog only returns the destination; creating the file is
             * the host's responsibility (file_dialog.h). */
            result = emit_single_uri(path, out_path, out_cap);
            CoTaskMemFree(path);
        }
        item->lpVtbl->Release(item);
    }

done:
    if (dlg)
        dlg->lpVtbl->Release(dlg);
    CoUninitialize();
    return result;
}

IRIS_API int iris_pick_files(const iris_file_dialog_opts *opts, char *out_paths, size_t out_cap,
                             size_t *out_bytes_used) {
    return pick_open(opts, true, false, out_paths, out_cap, out_bytes_used);
}
