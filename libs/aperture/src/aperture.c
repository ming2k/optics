/* aperture.c — lens bound to the iris UI-host seam (iris/host.h).
 *
 * Everything here is portable: no platform header, no backend #ifdef. The
 * three iris backends reach lens only through the function table built in
 * aperture_app_run. */

#include "host_binding.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/*  Vocabulary agreement                                               */
/* ------------------------------------------------------------------ */
/* iris and lens name the same things with their own constants so neither
 * includes the other. The conversions below are value copies; these
 * assertions are what make that sound. A change on either side fails here,
 * at compile time, in the one translation unit that sees both. */

#define AP_SAME(a, b) static_assert((int)(a) == (int)(b), #a " must equal " #b)

AP_SAME(IRIS_BUTTON_LEFT, LENS_MOUSE_LEFT);
AP_SAME(IRIS_BUTTON_RIGHT, LENS_MOUSE_RIGHT);
AP_SAME(IRIS_BUTTON_MIDDLE, LENS_MOUSE_MIDDLE);
AP_SAME(IRIS_BUTTON_COUNT, LENS_MOUSE_COUNT);

AP_SAME(IRIS_KEY_ESCAPE, LENS_KEY_ESCAPE);
AP_SAME(IRIS_KEY_TAB, LENS_KEY_TAB);
AP_SAME(IRIS_KEY_RETURN, LENS_KEY_RETURN);
AP_SAME(IRIS_KEY_BACKSPACE, LENS_KEY_BACKSPACE);
AP_SAME(IRIS_KEY_DELETE, LENS_KEY_DELETE);
AP_SAME(IRIS_KEY_LEFT, LENS_KEY_LEFT);
AP_SAME(IRIS_KEY_RIGHT, LENS_KEY_RIGHT);
AP_SAME(IRIS_KEY_HOME, LENS_KEY_HOME);
AP_SAME(IRIS_KEY_END, LENS_KEY_END);
AP_SAME(IRIS_KEY_UP, LENS_KEY_UP);
AP_SAME(IRIS_KEY_DOWN, LENS_KEY_DOWN);
AP_SAME(IRIS_KEY_F1, LENS_KEY_F1);
AP_SAME(IRIS_KEY_F12, LENS_KEY_F12);

AP_SAME(IRIS_MOD_SHIFT, LENS_MOD_SHIFT);
AP_SAME(IRIS_MOD_CTRL, LENS_MOD_CTRL);
AP_SAME(IRIS_MOD_ALT, LENS_MOD_ALT);
AP_SAME(IRIS_MOD_SUPER, LENS_MOD_SUPER);

AP_SAME(IRIS_INPUT_MAX_KEYS, LENS_INPUT_MAX_KEYS);
AP_SAME(IRIS_PREEDIT_MAX, LENS_PREEDIT_MAX);
AP_SAME(IRIS_DAMAGE_RECTS_CAPACITY, LENS_DAMAGE_RECTS_CAPACITY);
static_assert(sizeof((iris_input *)0)->text_utf8 == sizeof((lens_input *)0)->text_utf8,
              "committed-text buffers must match");
static_assert(sizeof((iris_input *)0)->preedit_utf8 == sizeof((lens_input *)0)->preedit_utf8,
              "preedit buffers must match");

AP_SAME(IRIS_ROLE_NONE, LENS_ROLE_NONE);
AP_SAME(IRIS_ROLE_GROUP, LENS_ROLE_GROUP);
AP_SAME(IRIS_ROLE_LABEL, LENS_ROLE_LABEL);
AP_SAME(IRIS_ROLE_BUTTON, LENS_ROLE_BUTTON);
AP_SAME(IRIS_ROLE_CHECKBOX, LENS_ROLE_CHECKBOX);
AP_SAME(IRIS_ROLE_SLIDER, LENS_ROLE_SLIDER);
AP_SAME(IRIS_ROLE_DISCLOSURE, LENS_ROLE_DISCLOSURE);
AP_SAME(IRIS_ROLE_SCROLLAREA, LENS_ROLE_SCROLLAREA);
AP_SAME(IRIS_ROLE_TEXTFIELD, LENS_ROLE_TEXTFIELD);
AP_SAME(IRIS_ROLE_TEXTAREA, LENS_ROLE_TEXTAREA);
AP_SAME(IRIS_ROLE_MENU, LENS_ROLE_MENU);
AP_SAME(IRIS_ROLE_RADIO, LENS_ROLE_RADIO);
AP_SAME(IRIS_ROLE_DIALOG, LENS_ROLE_DIALOG);
AP_SAME(IRIS_ROLE_PROGRESS, LENS_ROLE_PROGRESS);
AP_SAME(IRIS_ROLE_TABLE, LENS_ROLE_TABLE);
AP_SAME(IRIS_ROLE_ROW, LENS_ROLE_ROW);
AP_SAME(IRIS_ROLE_MENUITEM, LENS_ROLE_MENUITEM);
AP_SAME(IRIS_ROLE_LINK, LENS_ROLE_LINK);

AP_SAME(IRIS_A11Y_FOCUSED, LENS_A11Y_FOCUSED);
AP_SAME(IRIS_A11Y_DISABLED, LENS_A11Y_DISABLED);
AP_SAME(IRIS_A11Y_CHECKED, LENS_A11Y_CHECKED);
AP_SAME(IRIS_A11Y_EXPANDED, LENS_A11Y_EXPANDED);
AP_SAME(IRIS_A11Y_READONLY, LENS_A11Y_READONLY);
AP_SAME(IRIS_A11Y_SELECTED, LENS_A11Y_SELECTED);
static_assert(sizeof(iris_node_id) == sizeof(lens_id), "node ids must have one width");

/* ------------------------------------------------------------------ */
/*  State                                                              */
/* ------------------------------------------------------------------ */

/* ap_app is declared in host_binding.h. */

/* ------------------------------------------------------------------ */
/*  Theme policy                                                       */
/* ------------------------------------------------------------------ */

/* Raised-contrast variant of the current token set: white on black, full
 * strength borders. Applied as a mutation of the scheme preset so the result
 * is deliberate, not a second preset to keep in sync. */
static void ap_apply_theme(ap_app *app) {
    lens_theme th = app->dark ? lens_theme_dark() : lens_theme_default();
    if (app->high_contrast) {
        th.color_bg = flux_color_rgba(0x00, 0x00, 0x00, 0xff);
        th.color_fg = flux_color_rgba(0xff, 0xff, 0xff, 0xff);
        th.color_hover = flux_color_rgba(0x2a, 0x2a, 0x2a, 0xff);
        th.color_active = flux_color_rgba(0x55, 0x55, 0x55, 0xff);
        th.color_border = th.color_fg;
        th.color_disabled = flux_color_rgba(0xaa, 0xaa, 0xaa, 0xff);
    }
    lens_set_theme(app->ui, th);
}

/* ------------------------------------------------------------------ */
/*  lens → window services                                             */
/* ------------------------------------------------------------------ */

static void ap_clip_request_text(void *user) {
    ap_app *app = user;
    if (app->services.clipboard_request_text)
        app->services.clipboard_request_text(app->services.user);
}

static void ap_clip_set_text(const char *utf8, size_t len, void *user) {
    ap_app *app = user;
    if (app->services.clipboard_set_text)
        app->services.clipboard_set_text(utf8, len, app->services.user);
}

static int ap_start_drag(const char *text, size_t len, uint32_t actions, void *user) {
    ap_app *app = user;
    return app->services.start_drag
               ? app->services.start_drag(text, len, actions, app->services.user)
               : -1;
}

/* ------------------------------------------------------------------ */
/*  iris_ui_host implementation                                        */
/* ------------------------------------------------------------------ */

static bool ap_start(void *user, const iris_ui_start_info *info) {
    ap_app *app = user;
    app->device = info->device;
    app->services = info->services;
    app->dark = info->dark;

    lens_desc desc = LENS_DESC_INIT;
    desc.device = info->device;
    desc.theme = info->dark ? lens_theme_dark() : lens_theme_default();
    desc.scale = info->scale;
    desc.clipboard = (lens_clipboard){
        .request_text = ap_clip_request_text, .set_text = ap_clip_set_text, .user = app};
    desc.dnd = (lens_dnd_host){.start_drag = ap_start_drag, .user = app};
    if (lens_create(&desc, &app->ui) != FLUX_OK)
        return false;

    if (app->opts->start && !app->opts->start(app->ui, app->device, app->opts->user))
        return false;
    app->app_started = true;
    return true;
}

static void ap_stop(void *user) {
    ap_app *app = user;
    if (app->app_started && app->opts->stop)
        app->opts->stop(app->ui, app->device, app->opts->user);
    app->app_started = false;
}

static void ap_destroy(void *user) {
    ap_app *app = user;
    if (app->ui)
        lens_release(app->ui);
    app->ui = nullptr;
}

static iris_cursor ap_cursor(lens_cursor_hint hint) {
    switch (hint) {
    case LENS_CURSOR_POINTER:
        return IRIS_CURSOR_POINTER;
    case LENS_CURSOR_TEXT:
        return IRIS_CURSOR_TEXT;
    case LENS_CURSOR_RESIZE_EW:
        return IRIS_CURSOR_RESIZE_EW;
    case LENS_CURSOR_RESIZE_NS:
        return IRIS_CURSOR_RESIZE_NS;
    case LENS_CURSOR_DEFAULT:
    default:
        return IRIS_CURSOR_DEFAULT;
    }
}

static void ap_fold_input(const iris_input *in, lens_input *out) {
    *out = (lens_input){.size = sizeof *out};
    out->cursor = in->cursor;
    for (int i = 0; i < LENS_MOUSE_COUNT; i++) {
        out->mouse_down[i] = in->mouse_down[i];
        out->mouse_pressed[i] = in->mouse_pressed[i];
        out->mouse_released[i] = in->mouse_released[i];
    }
    out->scroll_x = in->scroll_x;
    out->scroll_y = in->scroll_y;
    out->scroll_pixels_x = in->scroll_pixels_x;
    out->scroll_pixels_y = in->scroll_pixels_y;
    out->mods = in->mods;
    memcpy(out->text_utf8, in->text_utf8, sizeof out->text_utf8);
    out->key_count = in->key_count < LENS_INPUT_MAX_KEYS ? in->key_count : LENS_INPUT_MAX_KEYS;
    for (uint32_t i = 0; i < out->key_count; i++)
        out->keys[i] = (lens_key_event){
            .key = in->keys[i].key, .pressed = in->keys[i].pressed, .repeat = in->keys[i].repeat};
    out->display_size = in->display_size;
    out->dt_seconds = in->dt_seconds;
    memcpy(out->preedit_utf8, in->preedit_utf8, sizeof out->preedit_utf8);
    out->preedit_cursor = in->preedit_cursor;
    out->preedit_sel_lo = in->preedit_sel_lo;
    out->preedit_sel_hi = in->preedit_sel_hi;
    out->ime_delete_before = in->ime_delete_before;
    out->ime_delete_after = in->ime_delete_after;
    out->pen_active = in->pen_active;
    out->pen_pressure = in->pen_pressure;
    out->pen_eraser = in->pen_eraser;
}

static void ap_frame(void *user, const iris_input *in, iris_ui_frame *out) {
    ap_app *app = user;
    lens_input li;
    ap_fold_input(in, &li);

    lens_begin(app->ui, &li);
    if (app->opts->build)
        app->opts->build(app->ui, &li, app->opts->user);
    lens_end(app->ui);

    const lens_text_context text = lens_text_context_get(app->ui);
    *out = (iris_ui_frame){
        .damaged = lens_frame_needs_repaint(app->ui),
        .animating = lens_anim_pending(app->ui),
        /* lens is the single authority for the blink phase (ADR-0151). */
        .caret_ticking = lens_caret_clock_live(app->ui),
        .cursor = ap_cursor(lens_get_cursor_hint(app->ui)),
        .text = {.utf8 = text.utf8,
                 .len = text.len,
                 .cursor = text.cursor,
                 .multiline = text.multiline,
                 .caret = lens_caret_rect(app->ui)},
        .clear_color = lens_get_theme(app->ui).color_bg,
    };
}

static void ap_set_scale(void *user, float scale) {
    ap_app *app = user;
    lens_set_scale(app->ui, scale);
}

static void ap_set_color_scheme(void *user, iris_color_scheme scheme) {
    ap_app *app = user;
    /* A forced-dark application does not follow the system. */
    if (app->opts->dark)
        return;
    app->dark = scheme != IRIS_COLOR_SCHEME_PREFER_LIGHT;
    ap_apply_theme(app);
}

static void ap_set_a11y_prefs(void *user, const iris_a11y_prefs *prefs) {
    ap_app *app = user;
    lens_set_reduced_motion(app->ui, prefs->reduced_motion);
    lens_set_text_scale(app->ui, prefs->text_scale);
    /* Touch the theme only on a contrast transition, so an application's own
     * token customisations survive an unrelated preference change. */
    if (prefs->high_contrast != app->high_contrast) {
        app->high_contrast = prefs->high_contrast;
        ap_apply_theme(app);
    }
}

static void ap_paste(void *user, const char *utf8, size_t len) {
    ap_app *app = user;
    lens_paste(app->ui, &(lens_paste_opts){.utf8 = utf8, .len = len});
}

static void ap_drop(void *user, const char *utf8, size_t len, flux_point pos) {
    ap_app *app = user;
    lens_deliver_drop(app->ui, &(lens_deliver_drop_opts){.payload = utf8, .len = len, .pos = pos});
}

static void ap_quiesce(void *user) {
    ap_app *app = user;
    /* The text engine's high-water scratch from a one-off huge paste can go
     * home while nothing is on screen to shape (ADR-0072). */
    lens_text_compact(app->ui);
}

static uint32_t ap_damage_rects(void *user, flux_rect *rects, uint32_t capacity) {
    ap_app *app = user;
    return lens_damage_rects(app->ui, &(lens_damage_opts){.rects = rects, .capacity = capacity});
}

/* iris_ui_snapshot is lens_scene_snapshot seen from below the seam. */
static flux_result ap_snapshot_create(void *user, iris_ui_snapshot **out) {
    ap_app *app = user;
    lens_scene_snapshot *snapshot = nullptr;
    const flux_result r = lens_snapshot_create(app->ui, &snapshot);
    *out = (iris_ui_snapshot *)snapshot;
    return r;
}

static flux_result ap_snapshot_submit(const iris_ui_snapshot *snapshot, flux_canvas *canvas) {
    return lens_snapshot_submit((const lens_scene_snapshot *)snapshot, canvas);
}

static flux_result ap_snapshot_activate(void *user, iris_ui_snapshot *snapshot) {
    ap_app *app = user;
    return lens_snapshot_activate(app->ui, (lens_scene_snapshot *)snapshot);
}

static iris_ui_snapshot *ap_snapshot_retain(iris_ui_snapshot *snapshot) {
    return (iris_ui_snapshot *)lens_snapshot_retain((lens_scene_snapshot *)snapshot);
}

static void ap_snapshot_release(iris_ui_snapshot *snapshot) {
    lens_snapshot_release((lens_scene_snapshot *)snapshot);
}

typedef struct ap_walk {
    iris_a11y_visit_fn visit;
    void *user;
} ap_walk;

static void ap_a11y_visit(const lens_semantics *s, flux_rect bounds, lens_id id, lens_id parent,
                          void *user) {
    const ap_walk *walk = user;
    const iris_semantics sem = {
        .role = (iris_role)s->role, .name = s->name, .value = s->value, .flags = s->flags};
    walk->visit(&sem, bounds, id, parent, walk->user);
}

static void ap_a11y_walk(void *user, iris_a11y_visit_fn visit, void *visit_user) {
    ap_app *app = user;
    ap_walk walk = {.visit = visit, .user = visit_user};
    lens_accessibility_walk(app->ui, ap_a11y_visit, &walk);
}

static void ap_a11y_activate(void *user, iris_node_id id) {
    ap_app *app = user;
    lens_a11y_activate(app->ui, id);
}

/* ------------------------------------------------------------------ */
/*  Entry point                                                        */
/* ------------------------------------------------------------------ */

iris_ui_host aperture__bind(ap_app *app, const aperture_app_opts *opts) {
    *app = (ap_app){.opts = opts};
    return (iris_ui_host){
        .user = app,
        .start = ap_start,
        .stop = ap_stop,
        .destroy = ap_destroy,
        .frame = ap_frame,
        .set_scale = ap_set_scale,
        .set_color_scheme = ap_set_color_scheme,
        .set_a11y_prefs = ap_set_a11y_prefs,
        .paste = ap_paste,
        .drop = ap_drop,
        .quiesce = ap_quiesce,
        .damage_rects = ap_damage_rects,
        .snapshot_create = ap_snapshot_create,
        .snapshot_submit = ap_snapshot_submit,
        .snapshot_activate = ap_snapshot_activate,
        .snapshot_retain = ap_snapshot_retain,
        .snapshot_release = ap_snapshot_release,
        .a11y_walk = ap_a11y_walk,
        .a11y_activate = ap_a11y_activate,
    };
}

APERTURE_API int aperture_app_run(const aperture_app_opts *opts) {
    if (!opts)
        return 1;

    ap_app app;
    const iris_ui_host host = aperture__bind(&app, opts);
    return iris_app_run(&(iris_app_opts){
        .title = opts->title,
        .app_id = opts->app_id,
        .width = opts->width,
        .height = opts->height,
        .dark = opts->dark,
        .ui = &host,
        .log_raw = opts->log_raw,
        .no_clear = opts->no_clear,
        .on_event = opts->on_event,
        .record = opts->record,
        .prepare = opts->prepare,
        .user = opts->user,
    });
}
