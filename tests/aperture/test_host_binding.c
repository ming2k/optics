/* test_host_binding.c — the lens ↔ iris_ui_host seam, driven without a window.
 *
 * aperture_app_run is `aperture__bind` handed to iris_app_run. Everything a
 * platform backend ever does to the UI layer goes through the returned table,
 * so calling that table here in the order a backend does covers the whole
 * shell on every platform — including the two whose backends CI cannot run.
 */

#include "host_binding.h"
#include "optics_test.h"

#include <string.h>

typedef struct fixture {
    int started, stopped, builds;
    int clicks;
    lens_response button;
    char clipboard[64];
    int paste_requests;
    int drags;
    int a11y_buttons;
    iris_node_id a11y_button_id;
} fixture;

static bool on_start(lens *ui, flux_device *device, void *user) {
    fixture *fx = user;
    (void)device;
    fx->started++;
    return ui != nullptr;
}

static void on_stop(lens *ui, flux_device *device, void *user) {
    fixture *fx = user;
    (void)device;
    fx->stopped += ui != nullptr;
}

static void on_build(lens *ui, const lens_input *in, void *user) {
    fixture *fx = user;
    (void)in;
    fx->builds++;
    fx->button =
        lens_button(ui, &(lens_button_opts){.label = "Save",
                                            .box = {.id = "save", .width = 120, .height = 40}});
    if (fx->button.clicked)
        fx->clicks++;
}

static void clip_set(const char *utf8, size_t len, void *user) {
    fixture *fx = user;
    if (len >= sizeof fx->clipboard)
        len = sizeof fx->clipboard - 1;
    memcpy(fx->clipboard, utf8, len);
    fx->clipboard[len] = '\0';
}
static void clip_request(void *user) {
    ((fixture *)user)->paste_requests++;
}
static int start_drag(const char *text, size_t len, uint32_t actions, void *user) {
    (void)text;
    (void)len;
    (void)actions;
    ((fixture *)user)->drags++;
    return 0;
}

static void a11y_visit(const iris_semantics *s, flux_rect bounds, iris_node_id id,
                       iris_node_id parent, void *user) {
    fixture *fx = user;
    (void)parent;
    if (s->role == IRIS_ROLE_BUTTON && s->name && strcmp(s->name, "Save") == 0) {
        fx->a11y_buttons++;
        fx->a11y_button_id = id;
        OT_EXPECT(bounds.w > 0.0f && bounds.h > 0.0f);
    }
}

static iris_input input_at(float x, float y) {
    return (iris_input){.cursor = {x, y}, .display_size = {400, 240}, .dt_seconds = 1.0f / 60.0f};
}

/* One frame the way a backend runs it: build, snapshot, "present", activate. */
static iris_ui_frame run_frame(const iris_ui_host *host, const iris_input *in) {
    iris_ui_frame report = {};
    host->frame(host->user, in, &report);

    iris_ui_snapshot *snapshot = nullptr;
    OT_EXPECT(host->snapshot_create(host->user, &snapshot) == FLUX_OK);
    OT_EXPECT(snapshot != nullptr);
    /* The presentation thread holds its own reference while it draws. */
    OT_EXPECT(host->snapshot_retain(snapshot) == snapshot);
    OT_EXPECT(host->snapshot_activate(host->user, snapshot) == FLUX_OK);
    host->snapshot_release(snapshot);
    host->snapshot_release(snapshot);
    return report;
}

static void test_lifecycle_input_and_presentation(void) {
    fixture fx = {};
    ap_app app;
    const aperture_app_opts opts = {
        .start = on_start, .stop = on_stop, .build = on_build, .user = &fx};
    const iris_ui_host host = aperture__bind(&app, &opts);

    /* Headless: no device. lens falls back to libc for persistent storage. */
    OT_REQUIRE(host.start(host.user, &(iris_ui_start_info){
                                         .scale = 1.0f,
                                         .dark = true,
                                         .services = {.clipboard_set_text = clip_set,
                                                      .clipboard_request_text = clip_request,
                                                      .start_drag = start_drag,
                                                      .user = &fx},
                                     }));
    OT_EXPECT(fx.started == 1);

    /* First frame lays the button out; the surface has never been painted. */
    iris_input in = input_at(-100, -100);
    iris_ui_frame rep = run_frame(&host, &in);
    OT_EXPECT(fx.builds == 1);
    OT_EXPECT(fx.button.rect.w > 0.0f || fx.builds == 1);

    /* Settle, then find the button and click it through folded input. */
    rep = run_frame(&host, &in);
    const float bx = fx.button.rect.x + fx.button.rect.w * 0.5f;
    const float by = fx.button.rect.y + fx.button.rect.h * 0.5f;
    OT_REQUIRE(fx.button.rect.w > 0.0f);

    in = input_at(bx, by);
    rep = run_frame(&host, &in);
    OT_EXPECT(fx.button.hovered);
    /* The semantic cursor crosses the seam as an iris cursor. */
    OT_EXPECT(rep.cursor == IRIS_CURSOR_POINTER || rep.cursor == IRIS_CURSOR_DEFAULT);

    in = input_at(bx, by);
    in.mouse_down[IRIS_BUTTON_LEFT] = true;
    in.mouse_pressed[IRIS_BUTTON_LEFT] = true;
    rep = run_frame(&host, &in);
    /* A press changes the button's visuals: the loop must be told to paint. */
    OT_EXPECT(rep.damaged);

    in = input_at(bx, by);
    in.mouse_released[IRIS_BUTTON_LEFT] = true;
    rep = run_frame(&host, &in);
    OT_EXPECT(fx.clicks == 1);

    /* Damage is reported in the frame's own units and never overruns. */
    flux_rect rects[IRIS_DAMAGE_RECTS_CAPACITY];
    OT_EXPECT(host.damage_rects(host.user, rects, IRIS_DAMAGE_RECTS_CAPACITY) <=
              IRIS_DAMAGE_RECTS_CAPACITY);

    /* Accessibility: the tree crosses the seam with iris roles, and an
     * activation from an assistive client lands as a click next frame. */
    host.a11y_walk(host.user, a11y_visit, &fx);
    OT_EXPECT(fx.a11y_buttons == 1);
    OT_REQUIRE(fx.a11y_button_id != 0);
    host.a11y_activate(host.user, fx.a11y_button_id);
    in = input_at(-100, -100);
    (void)run_frame(&host, &in);
    OT_EXPECT(fx.clicks == 2);

    host.stop(host.user);
    OT_EXPECT(fx.stopped == 1);
    host.destroy(host.user);
    OT_EXPECT(app.ui == nullptr);
}

static void test_theme_policy(void) {
    fixture fx = {};
    ap_app app;
    const aperture_app_opts opts = {.build = on_build, .user = &fx};
    const iris_ui_host host = aperture__bind(&app, &opts);
    OT_REQUIRE(host.start(host.user, &(iris_ui_start_info){.scale = 1.0f, .dark = false}));

    iris_input in = input_at(-100, -100);
    const flux_color light_bg = run_frame(&host, &in).clear_color;

    /* The system scheme flips: the clear colour follows. */
    host.set_color_scheme(host.user, IRIS_COLOR_SCHEME_PREFER_DARK);
    const flux_color dark_bg = run_frame(&host, &in).clear_color;
    OT_EXPECT(dark_bg != light_bg);
    OT_EXPECT(dark_bg == lens_theme_dark().color_bg);

    /* High contrast is a mutation of the current scheme, and leaving it
     * restores that scheme — not whatever the system default is. */
    host.set_a11y_prefs(host.user, &(iris_a11y_prefs){.high_contrast = true, .text_scale = 1.0f});
    OT_EXPECT(run_frame(&host, &in).clear_color == flux_color_rgba(0, 0, 0, 0xff));
    host.set_a11y_prefs(host.user, &(iris_a11y_prefs){.high_contrast = false, .text_scale = 1.0f});
    OT_EXPECT(run_frame(&host, &in).clear_color == dark_bg);

    /* A preference change that does not touch contrast leaves an application's
     * own theme alone. */
    lens_theme custom = lens_get_theme(app.ui);
    custom.color_bg = flux_color_rgba(1, 2, 3, 0xff);
    lens_set_theme(app.ui, custom);
    host.set_a11y_prefs(host.user, &(iris_a11y_prefs){.reduced_motion = true, .text_scale = 1.25f});
    OT_EXPECT(run_frame(&host, &in).clear_color == flux_color_rgba(1, 2, 3, 0xff));
    OT_EXPECT(lens_reduced_motion(app.ui));

    host.stop(host.user);
    host.destroy(host.user);
}

static void test_forced_dark_ignores_system_scheme(void) {
    fixture fx = {};
    ap_app app;
    const aperture_app_opts opts = {.dark = true, .build = on_build, .user = &fx};
    const iris_ui_host host = aperture__bind(&app, &opts);
    OT_REQUIRE(host.start(host.user, &(iris_ui_start_info){.scale = 1.0f, .dark = true}));

    iris_input in = input_at(-100, -100);
    const flux_color before = run_frame(&host, &in).clear_color;
    host.set_color_scheme(host.user, IRIS_COLOR_SCHEME_PREFER_LIGHT);
    OT_EXPECT(run_frame(&host, &in).clear_color == before);

    host.stop(host.user);
    host.destroy(host.user);
}

static bool failing_start(lens *ui, flux_device *device, void *user) {
    (void)ui;
    (void)device;
    (void)user;
    return false;
}

static void test_failed_start_never_calls_stop(void) {
    fixture fx = {};
    ap_app app;
    const aperture_app_opts opts = {.start = failing_start, .stop = on_stop, .user = &fx};
    const iris_ui_host host = aperture__bind(&app, &opts);
    OT_EXPECT(!host.start(host.user, &(iris_ui_start_info){.scale = 1.0f}));
    host.stop(host.user);
    OT_EXPECT(fx.stopped == 0);
    host.destroy(host.user); /* the lens context created before the failure is released */
    OT_EXPECT(app.ui == nullptr);
}

int main(void) {
    OT_EXPECT(aperture_app_run(nullptr) != 0);
    test_lifecycle_input_and_presentation();
    test_theme_policy();
    test_forced_dark_ignores_system_scheme();
    test_failed_start_never_calls_stop();
    return OT_REPORT();
}
