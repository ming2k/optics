/* app.c — ui bound to the window UI-host seam (window/host.h).
 *
 * Everything here is portable: no platform header, no backend #ifdef. The
 * three window backends reach ui only through the function table built in
 * optics_app_run. */

#include "host_binding.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/*  Vocabulary agreement                                               */
/* ------------------------------------------------------------------ */
/* window and ui name the same things with their own constants so neither
 * includes the other. The conversions below are value copies; these
 * assertions are what make that sound. A change on either side fails here,
 * at compile time, in the one translation unit that sees both. */

#define AP_SAME(a, b) static_assert((int)(a) == (int)(b), #a " must equal " #b)

AP_SAME(OPTICS_WINDOW_BUTTON_LEFT, OPTICS_UI_MOUSE_LEFT);
AP_SAME(OPTICS_WINDOW_BUTTON_RIGHT, OPTICS_UI_MOUSE_RIGHT);
AP_SAME(OPTICS_WINDOW_BUTTON_MIDDLE, OPTICS_UI_MOUSE_MIDDLE);
AP_SAME(OPTICS_WINDOW_BUTTON_COUNT, OPTICS_UI_MOUSE_COUNT);

AP_SAME(OPTICS_WINDOW_KEY_ESCAPE, OPTICS_UI_KEY_ESCAPE);
AP_SAME(OPTICS_WINDOW_KEY_TAB, OPTICS_UI_KEY_TAB);
AP_SAME(OPTICS_WINDOW_KEY_RETURN, OPTICS_UI_KEY_RETURN);
AP_SAME(OPTICS_WINDOW_KEY_BACKSPACE, OPTICS_UI_KEY_BACKSPACE);
AP_SAME(OPTICS_WINDOW_KEY_DELETE, OPTICS_UI_KEY_DELETE);
AP_SAME(OPTICS_WINDOW_KEY_LEFT, OPTICS_UI_KEY_LEFT);
AP_SAME(OPTICS_WINDOW_KEY_RIGHT, OPTICS_UI_KEY_RIGHT);
AP_SAME(OPTICS_WINDOW_KEY_HOME, OPTICS_UI_KEY_HOME);
AP_SAME(OPTICS_WINDOW_KEY_END, OPTICS_UI_KEY_END);
AP_SAME(OPTICS_WINDOW_KEY_UP, OPTICS_UI_KEY_UP);
AP_SAME(OPTICS_WINDOW_KEY_DOWN, OPTICS_UI_KEY_DOWN);
AP_SAME(OPTICS_WINDOW_KEY_F1, OPTICS_UI_KEY_F1);
AP_SAME(OPTICS_WINDOW_KEY_F12, OPTICS_UI_KEY_F12);

AP_SAME(OPTICS_WINDOW_MOD_SHIFT, OPTICS_UI_MOD_SHIFT);
AP_SAME(OPTICS_WINDOW_MOD_CTRL, OPTICS_UI_MOD_CTRL);
AP_SAME(OPTICS_WINDOW_MOD_ALT, OPTICS_UI_MOD_ALT);
AP_SAME(OPTICS_WINDOW_MOD_SUPER, OPTICS_UI_MOD_SUPER);

AP_SAME(OPTICS_WINDOW_INPUT_MAX_KEYS, OPTICS_UI_INPUT_MAX_KEYS);
AP_SAME(OPTICS_WINDOW_PREEDIT_MAX, OPTICS_UI_PREEDIT_MAX);
AP_SAME(OPTICS_WINDOW_DAMAGE_RECTS_CAPACITY, OPTICS_UI_DAMAGE_RECTS_CAPACITY);
static_assert(sizeof((optics_window_input *)0)->text_utf8 ==
                  sizeof((optics_ui_input *)0)->text_utf8,
              "committed-text buffers must match");
static_assert(sizeof((optics_window_input *)0)->preedit_utf8 ==
                  sizeof((optics_ui_input *)0)->preedit_utf8,
              "preedit buffers must match");

AP_SAME(OPTICS_WINDOW_ROLE_NONE, OPTICS_UI_ROLE_NONE);
AP_SAME(OPTICS_WINDOW_ROLE_GROUP, OPTICS_UI_ROLE_GROUP);
AP_SAME(OPTICS_WINDOW_ROLE_LABEL, OPTICS_UI_ROLE_LABEL);
AP_SAME(OPTICS_WINDOW_ROLE_BUTTON, OPTICS_UI_ROLE_BUTTON);
AP_SAME(OPTICS_WINDOW_ROLE_CHECKBOX, OPTICS_UI_ROLE_CHECKBOX);
AP_SAME(OPTICS_WINDOW_ROLE_SLIDER, OPTICS_UI_ROLE_SLIDER);
AP_SAME(OPTICS_WINDOW_ROLE_DISCLOSURE, OPTICS_UI_ROLE_DISCLOSURE);
AP_SAME(OPTICS_WINDOW_ROLE_SCROLLAREA, OPTICS_UI_ROLE_SCROLLAREA);
AP_SAME(OPTICS_WINDOW_ROLE_TEXTFIELD, OPTICS_UI_ROLE_TEXTFIELD);
AP_SAME(OPTICS_WINDOW_ROLE_TEXTAREA, OPTICS_UI_ROLE_TEXTAREA);
AP_SAME(OPTICS_WINDOW_ROLE_MENU, OPTICS_UI_ROLE_MENU);
AP_SAME(OPTICS_WINDOW_ROLE_RADIO, OPTICS_UI_ROLE_RADIO);
AP_SAME(OPTICS_WINDOW_ROLE_DIALOG, OPTICS_UI_ROLE_DIALOG);
AP_SAME(OPTICS_WINDOW_ROLE_PROGRESS, OPTICS_UI_ROLE_PROGRESS);
AP_SAME(OPTICS_WINDOW_ROLE_TABLE, OPTICS_UI_ROLE_TABLE);
AP_SAME(OPTICS_WINDOW_ROLE_ROW, OPTICS_UI_ROLE_ROW);
AP_SAME(OPTICS_WINDOW_ROLE_MENUITEM, OPTICS_UI_ROLE_MENUITEM);
AP_SAME(OPTICS_WINDOW_ROLE_LINK, OPTICS_UI_ROLE_LINK);

AP_SAME(OPTICS_WINDOW_A11Y_FOCUSED, OPTICS_UI_A11Y_FOCUSED);
AP_SAME(OPTICS_WINDOW_A11Y_DISABLED, OPTICS_UI_A11Y_DISABLED);
AP_SAME(OPTICS_WINDOW_A11Y_CHECKED, OPTICS_UI_A11Y_CHECKED);
AP_SAME(OPTICS_WINDOW_A11Y_EXPANDED, OPTICS_UI_A11Y_EXPANDED);
AP_SAME(OPTICS_WINDOW_A11Y_READONLY, OPTICS_UI_A11Y_READONLY);
AP_SAME(OPTICS_WINDOW_A11Y_SELECTED, OPTICS_UI_A11Y_SELECTED);
static_assert(sizeof(optics_window_node_id) == sizeof(optics_ui_id),
              "node ids must have one width");

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
    optics_ui_theme th = app->dark ? optics_ui_theme_dark() : optics_ui_theme_default();
    if (app->high_contrast) {
        th.color_bg = optics_gfx_color_rgba(0x00, 0x00, 0x00, 0xff);
        th.color_fg = optics_gfx_color_rgba(0xff, 0xff, 0xff, 0xff);
        th.color_hover = optics_gfx_color_rgba(0x2a, 0x2a, 0x2a, 0xff);
        th.color_active = optics_gfx_color_rgba(0x55, 0x55, 0x55, 0xff);
        th.color_border = th.color_fg;
        th.color_disabled = optics_gfx_color_rgba(0xaa, 0xaa, 0xaa, 0xff);
    }
    optics_ui_set_theme(app->ui, th);
}

/* ------------------------------------------------------------------ */
/*  ui → window services                                             */
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
/*  optics_window_ui_host implementation                                        */
/* ------------------------------------------------------------------ */

static bool ap_start(void *user, const optics_window_ui_start_info *info) {
    ap_app *app = user;
    app->device = info->device;
    app->services = info->services;
    app->dark = info->dark;

    optics_ui_desc desc = OPTICS_UI_DESC_INIT;
    desc.device = info->device;
    desc.theme = info->dark ? optics_ui_theme_dark() : optics_ui_theme_default();
    desc.scale = info->scale;
    desc.clipboard = (optics_ui_clipboard){
        .request_text = ap_clip_request_text, .set_text = ap_clip_set_text, .user = app};
    desc.dnd = (optics_ui_dnd_host){.start_drag = ap_start_drag, .user = app};
    if (optics_ui_create(&desc, &app->ui) != OPTICS_GFX_OK)
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
        optics_ui_release(app->ui);
    app->ui = nullptr;
}

static optics_window_cursor ap_cursor(optics_ui_cursor_hint hint) {
    switch (hint) {
    case OPTICS_UI_CURSOR_POINTER:
        return OPTICS_WINDOW_CURSOR_POINTER;
    case OPTICS_UI_CURSOR_TEXT:
        return OPTICS_WINDOW_CURSOR_TEXT;
    case OPTICS_UI_CURSOR_RESIZE_EW:
        return OPTICS_WINDOW_CURSOR_RESIZE_EW;
    case OPTICS_UI_CURSOR_RESIZE_NS:
        return OPTICS_WINDOW_CURSOR_RESIZE_NS;
    case OPTICS_UI_CURSOR_DEFAULT:
    default:
        return OPTICS_WINDOW_CURSOR_DEFAULT;
    }
}

static void ap_fold_input(const optics_window_input *in, optics_ui_input *out) {
    *out = (optics_ui_input){.size = sizeof *out};
    out->cursor = in->cursor;
    for (int i = 0; i < OPTICS_UI_MOUSE_COUNT; i++) {
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
    out->key_count =
        in->key_count < OPTICS_UI_INPUT_MAX_KEYS ? in->key_count : OPTICS_UI_INPUT_MAX_KEYS;
    for (uint32_t i = 0; i < out->key_count; i++)
        out->keys[i] = (optics_ui_key_event){
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

static void ap_frame(void *user, const optics_window_input *in, optics_window_ui_frame *out) {
    ap_app *app = user;
    optics_ui_input li;
    ap_fold_input(in, &li);

    optics_ui_begin(app->ui, &li);
    if (app->opts->build)
        app->opts->build(app->ui, &li, app->opts->user);
    optics_ui_end(app->ui);

    const optics_ui_text_context text = optics_ui_text_context_get(app->ui);
    *out = (optics_window_ui_frame){
        .damaged = optics_ui_frame_needs_repaint(app->ui),
        .animating = optics_ui_anim_pending(app->ui),
        /* ui is the single authority for the blink phase (ADR-0151). */
        .caret_ticking = optics_ui_caret_clock_live(app->ui),
        .cursor = ap_cursor(optics_ui_get_cursor_hint(app->ui)),
        .text = {.utf8 = text.utf8,
                 .len = text.len,
                 .cursor = text.cursor,
                 .multiline = text.multiline,
                 .caret = optics_ui_caret_rect(app->ui)},
        .clear_color = optics_ui_get_theme(app->ui).color_bg,
    };
}

static void ap_set_scale(void *user, float scale) {
    ap_app *app = user;
    optics_ui_set_scale(app->ui, scale);
}

static void ap_set_color_scheme(void *user, optics_window_color_scheme scheme) {
    ap_app *app = user;
    /* A forced-dark application does not follow the system. */
    if (app->opts->dark)
        return;
    app->dark = scheme != OPTICS_WINDOW_COLOR_SCHEME_PREFER_LIGHT;
    ap_apply_theme(app);
}

static void ap_set_a11y_prefs(void *user, const optics_window_a11y_prefs *prefs) {
    ap_app *app = user;
    optics_ui_set_reduced_motion(app->ui, prefs->reduced_motion);
    optics_ui_set_text_scale(app->ui, prefs->text_scale);
    /* Touch the theme only on a contrast transition, so an application's own
     * token customisations survive an unrelated preference change. */
    if (prefs->high_contrast != app->high_contrast) {
        app->high_contrast = prefs->high_contrast;
        ap_apply_theme(app);
    }
}

static void ap_paste(void *user, const char *utf8, size_t len) {
    ap_app *app = user;
    optics_ui_paste(app->ui, &(optics_ui_paste_opts){.utf8 = utf8, .len = len});
}

static void ap_drop(void *user, const char *utf8, size_t len, optics_gfx_point pos) {
    ap_app *app = user;
    optics_ui_deliver_drop(app->ui,
                           &(optics_ui_deliver_drop_opts){.payload = utf8, .len = len, .pos = pos});
}

static void ap_quiesce(void *user) {
    ap_app *app = user;
    /* The text engine's high-water scratch from a one-off huge paste can go
     * home while nothing is on screen to shape (ADR-0072). */
    optics_ui_text_compact(app->ui);
}

static uint32_t ap_damage_rects(void *user, optics_gfx_rect *rects, uint32_t capacity) {
    ap_app *app = user;
    return optics_ui_damage_rects(app->ui,
                                  &(optics_ui_damage_opts){.rects = rects, .capacity = capacity});
}

/* optics_window_ui_snapshot is optics_ui_scene_snapshot seen from below the seam. */
static optics_gfx_result ap_snapshot_create(void *user, optics_window_ui_snapshot **out) {
    ap_app *app = user;
    optics_ui_scene_snapshot *snapshot = nullptr;
    const optics_gfx_result r = optics_ui_snapshot_create(app->ui, &snapshot);
    *out = (optics_window_ui_snapshot *)snapshot;
    return r;
}

static optics_gfx_result ap_snapshot_submit(const optics_window_ui_snapshot *snapshot,
                                            optics_gfx_canvas *canvas) {
    return optics_ui_snapshot_submit((const optics_ui_scene_snapshot *)snapshot, canvas);
}

static optics_gfx_result ap_snapshot_activate(void *user, optics_window_ui_snapshot *snapshot) {
    ap_app *app = user;
    return optics_ui_snapshot_activate(app->ui, (optics_ui_scene_snapshot *)snapshot);
}

static optics_window_ui_snapshot *ap_snapshot_retain(optics_window_ui_snapshot *snapshot) {
    return (optics_window_ui_snapshot *)optics_ui_snapshot_retain(
        (optics_ui_scene_snapshot *)snapshot);
}

static void ap_snapshot_release(optics_window_ui_snapshot *snapshot) {
    optics_ui_snapshot_release((optics_ui_scene_snapshot *)snapshot);
}

typedef struct ap_walk {
    optics_window_a11y_visit_fn visit;
    void *user;
} ap_walk;

static void ap_a11y_visit(const optics_ui_semantics *s, optics_gfx_rect bounds, optics_ui_id id,
                          optics_ui_id parent, void *user) {
    const ap_walk *walk = user;
    const optics_window_semantics sem = {
        .role = (optics_window_role)s->role, .name = s->name, .value = s->value, .flags = s->flags};
    walk->visit(&sem, bounds, id, parent, walk->user);
}

static void ap_a11y_walk(void *user, optics_window_a11y_visit_fn visit, void *visit_user) {
    ap_app *app = user;
    ap_walk walk = {.visit = visit, .user = visit_user};
    optics_ui_accessibility_walk(app->ui, ap_a11y_visit, &walk);
}

static void ap_a11y_activate(void *user, optics_window_node_id id) {
    ap_app *app = user;
    optics_ui_a11y_activate(app->ui, id);
}

/* ------------------------------------------------------------------ */
/*  Entry point                                                        */
/* ------------------------------------------------------------------ */

optics_window_ui_host optics_app__bind(ap_app *app, const optics_app_opts *opts) {
    *app = (ap_app){.opts = opts};
    return (optics_window_ui_host){
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

OPTICS_APP_API int optics_app_run(const optics_app_opts *opts) {
    if (!opts)
        return 1;

    ap_app app;
    const optics_window_ui_host host = optics_app__bind(&app, opts);
    return optics_window_run(&(optics_window_opts){
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

OPTICS_APP_API void optics_app_quit(void) {
    optics_window_close();
}

OPTICS_APP_API void optics_app_request_frame(void) {
    optics_window_request_animation_frame();
}

OPTICS_APP_API int optics_app_post(optics_app_main_fn fn, void *user) {
    return optics_window_post_to_main_thread(fn, user);
}

OPTICS_APP_API bool optics_app_get_geometry(int32_t *out_width, int32_t *out_height) {
    return optics_window_get_geometry(out_width, out_height);
}
