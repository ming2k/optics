/* file_dialog_cocoa.m — file picker via NSOpenPanel / NSSavePanel.
 *
 * Implements iris/file_dialog.h on macOS. runModal blocks until the user
 * confirms or cancels, matching the portal backend's blocking contract.
 * Results are panel URLs' absoluteString — already file:// URIs, which is
 * exactly the iris_pick_* output format.
 *
 * Compiled with ARC (see the darwin branch in libs/iris/meson.build).
 *
 * UNVERIFIED ASSUMPTIONS (no Apple SDK on the authoring machine):
 *  1. Filters: iris glob patterns ("*.txt;*.md") are lowered to plain
 *     extensions for NSOpenPanel.allowedFileTypes. allowedFileTypes is
 *     deprecated since macOS 12 (in favour of UTType content types) but
 *     still functional and far simpler for extension-only filters; a
 *     pattern that is not an extension glob ("*", "Makefile") is ignored,
 *     so a filters array with no usable extension means "any file".
 *  2. The filter array is NULL-terminated on `name`, matching how
 *     file_dialog_portal.c iterates it.
 *  3. runModal from inside iris_app_run's hand-rolled loop stalls frame
 *     pacing for the duration of the dialog (same as the portal backend's
 *     modal loop). Wakeup/paste ApplicationDefined events queued meanwhile
 *     are drained after the dialog closes.
 */

#import <Cocoa/Cocoa.h>

#include <iris/file_dialog.h>

#include <string.h>

/* ------------------------------------------------------------------ */
/*  Options → panel                                                    */
/* ------------------------------------------------------------------ */

/* Lower the NULL-terminated iris_file_filter array to a flat extension
 * list. Returns nil when there is nothing usable (meaning "any file"). */
static NSArray<NSString *> *allowed_types_from_filters(const iris_file_filter *filters) {
    if (!filters)
        return nil;
    NSMutableArray<NSString *> *exts = [NSMutableArray array];
    /* NULL-terminated on name — same iteration rule as file_dialog_portal.c. */
    for (const iris_file_filter *f = filters; f->name; f++) {
        if (!f->pattern || !*f->pattern)
            continue;
        NSString *pattern = [NSString stringWithUTF8String:f->pattern];
        if (!pattern)
            continue;
        for (NSString *glob in [pattern componentsSeparatedByString:@";"]) {
            NSString *g =
                [glob stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
            if ([g hasPrefix:@"*."]) {
                NSString *ext = [g substringFromIndex:2];
                if ([ext length] > 0 && ![ext containsString:@"*"])
                    [exts addObject:ext];
            } else if ([g length] > 0 && ![g containsString:@"*"] && ![g containsString:@"/"]) {
                /* A bare word is treated as an extension already. */
                [exts addObject:g];
            }
            /* "*", path globs, and everything else: no restriction. */
        }
    }
    return [exts count] > 0 ? exts : nil;
}

static void apply_opts(NSSavePanel *panel, const iris_file_dialog_opts *opts) {
    if (!opts)
        return;
    if (opts->title) {
        NSString *title = [NSString stringWithUTF8String:opts->title];
        if (title)
            [panel setTitle:title];
    }
    if (opts->initial_uri) {
        NSString *uri = [NSString stringWithUTF8String:opts->initial_uri];
        NSURL *url = uri ? [NSURL URLWithString:uri] : nil;
        if (![url isFileURL] && uri)
            url = [NSURL fileURLWithPath:uri]; /* tolerate a plain path */
        if (url)
            [panel setDirectoryURL:url];
    }
    NSArray<NSString *> *types = allowed_types_from_filters(opts->filters);
    if (types)
        [panel setAllowedFileTypes:types];
}

/* ------------------------------------------------------------------ */
/*  Result copying                                                     */
/* ------------------------------------------------------------------ */

static int copy_uri(NSURL *url, char *out_path, size_t out_cap) {
    if (!out_path || out_cap == 0)
        return 1;
    const char *utf8 = [[url absoluteString] UTF8String];
    if (!utf8)
        return 1;
    size_t n = strlen(utf8);
    if (n + 1 > out_cap)
        return 1;
    memcpy(out_path, utf8, n + 1);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

IRIS_API int iris_pick_file(const iris_file_dialog_opts *opts, char *out_path, size_t out_cap) {
    @autoreleasepool {
        NSOpenPanel *panel = [NSOpenPanel openPanel];
        [panel setCanChooseFiles:!opts || !opts->directory_only];
        [panel setCanChooseDirectories:opts && opts->directory_only];
        [panel setAllowsMultipleSelection:NO];
        apply_opts(panel, opts);
        if ([panel runModal] != NSModalResponseOK)
            return 1;
        return copy_uri([panel URL], out_path, out_cap);
    }
}

IRIS_API int iris_pick_folder(const iris_file_dialog_opts *opts, char *out_path, size_t out_cap) {
    @autoreleasepool {
        NSOpenPanel *panel = [NSOpenPanel openPanel];
        [panel setCanChooseFiles:NO];
        [panel setCanChooseDirectories:YES];
        [panel setAllowsMultipleSelection:NO];
        apply_opts(panel, opts);
        if ([panel runModal] != NSModalResponseOK)
            return 1;
        return copy_uri([panel URL], out_path, out_cap);
    }
}

IRIS_API int iris_pick_save_path(const iris_file_dialog_opts *opts, const char *default_name,
                                 char *out_path, size_t out_cap) {
    @autoreleasepool {
        NSSavePanel *panel = [NSSavePanel savePanel];
        if (default_name) {
            NSString *name = [NSString stringWithUTF8String:default_name];
            if (name)
                [panel setNameFieldStringValue:name];
        }
        apply_opts(panel, opts);
        if ([panel runModal] != NSModalResponseOK)
            return 1;
        return copy_uri([panel URL], out_path, out_cap);
    }
}

IRIS_API int iris_pick_files(const iris_file_dialog_opts *opts, char *out_paths, size_t out_cap,
                             size_t *out_bytes_used) {
    @autoreleasepool {
        if (!out_paths || out_cap == 0)
            return 0;
        NSOpenPanel *panel = [NSOpenPanel openPanel];
        [panel setCanChooseFiles:YES];
        [panel setCanChooseDirectories:opts && opts->directory_only];
        [panel setAllowsMultipleSelection:YES];
        apply_opts(panel, opts);
        if ([panel runModal] != NSModalResponseOK)
            return 0;

        /* NUL-separated URIs; out_bytes_used excludes the trailing NUL. */
        size_t used = 0;
        int count = 0;
        for (NSURL *url in [panel URLs]) {
            const char *utf8 = [[url absoluteString] UTF8String];
            if (!utf8)
                continue;
            size_t n = strlen(utf8);
            if (used + n + 1 > out_cap)
                return 0; /* buffer too small: fail rather than truncate */
            memcpy(out_paths + used, utf8, n + 1);
            used += n + 1;
            count++;
        }
        if (count == 0)
            return 0;
        if (out_bytes_used)
            *out_bytes_used = used - 1; /* exclude the trailing terminator */
        return count;
    }
}
