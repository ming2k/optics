/* test_app_api.c — argument validation on the public entry points.
 *
 * Headless: every call here must fail fast WITHOUT opening a window or
 * connecting to a compositor.
 *
 *   - optics_window_run(NULL) is rejected at the dispatcher before any backend
 *     work. (A non-NULL config always dispatches to the live backend and
 *     opens a window, so it is NOT exercised here.)
 *   - file and folder pickers reject a NULL buffer / zero capacity without
 *     contacting the desktop portal.
 */

#include "optics_test.h"
#include <optics/window.h>

static bool host_start(optics_gfx_device *device, void *user) {
    return device != nullptr && user != nullptr;
}

static void host_stop(optics_gfx_device *device, void *user) {
    (void)device;
    (void)user;
}

static void host_record(optics_gfx_encoder *enc, float scale, void *user) {
    (void)enc;
    (void)scale;
    (void)user;
}

int main(void) {
    /* Lifecycle callbacks are part of the public, designated-init API.
     * Do not run this config headlessly; assigning it catches declaration
     * and function-signature regressions at compile time. */
    int user = 1;
    optics_window_opts lifecycle_config = {
        .start = host_start,
        .stop = host_stop,
        .record = host_record,
        .user = &user,
    };
    OT_EXPECT(lifecycle_config.start != nullptr);
    OT_EXPECT(lifecycle_config.stop != nullptr);
    OT_EXPECT(lifecycle_config.record != nullptr);

    /* optics_window_run: NULL config is rejected before any backend work. */
    OT_EXPECT(optics_window_run(nullptr) != 0);

    /* Safe outside a running app; the dispatcher/backend seam treats this
     * exactly like a request made after the window has closed. */
    optics_window_request_animation_frame();

    /* optics_window_record_mark_static: same headless contract — a no-op without an
     * active app, and never a crash. */
    optics_window_record_mark_static();

    /* DnD safety outside a running app (ADR-0086) */
    OT_EXPECT(!optics_window_dnd_is_active());
    optics_window_dnd_cancel();
    OT_EXPECT(optics_window_dnd_start(nullptr) != 0);

    /* Pickers reject NULL buffers / zero capacity before contacting the
     * portal. A valid buffer would open a live desktop picker. */
    OT_EXPECT(optics_window_pick_file(nullptr) != 0);
    char buf[16];
    OT_EXPECT(optics_window_pick_file(
                  &(optics_window_file_dialog_opts){.out_path = buf, .out_cap = 0}) != 0);
    OT_EXPECT(optics_window_pick_file(&(optics_window_file_dialog_opts){
                  .out_path = nullptr, .out_cap = sizeof buf}) != 0);

    /* Accessibility headless safety and lazy active query (ADR-0103) */
    OT_EXPECT(optics_window_a11y_update(nullptr) != 0);
    bool active_before = optics_window_a11y_is_active();
    (void)active_before;
    optics_window_a11y_shutdown();
    OT_EXPECT(!optics_window_a11y_is_active());
    OT_EXPECT(optics_window_pick_folder(nullptr) != 0);
    OT_EXPECT(optics_window_pick_folder(
                  &(optics_window_file_dialog_opts){.out_path = buf, .out_cap = 0}) != 0);
    OT_EXPECT(optics_window_pick_folder(&(optics_window_file_dialog_opts){
                  .out_path = nullptr, .out_cap = sizeof buf}) != 0);
    OT_EXPECT(optics_window_pick_save_path(nullptr) != 0);
    OT_EXPECT(optics_window_pick_save_path(
                  &(optics_window_file_dialog_opts){.out_path = buf, .out_cap = 0}) != 0);
    OT_EXPECT(optics_window_pick_save_path(&(optics_window_file_dialog_opts){
                  .out_path = nullptr, .out_cap = sizeof buf}) != 0);
    OT_EXPECT(optics_window_pick_files(nullptr) != 0);
    OT_EXPECT(optics_window_pick_files(
                  &(optics_window_file_dialog_opts){.out_path = buf, .out_cap = 0}) != 0);
    OT_EXPECT(optics_window_pick_files(&(optics_window_file_dialog_opts){
                  .out_path = nullptr, .out_cap = sizeof buf}) != 0);

    /* Activation token creation outside a running app returns -1 (ADR-0099) */
    char tok_buf[64];
    OT_EXPECT(optics_window_create_activation_token(nullptr) != 0);
    OT_EXPECT(optics_window_create_activation_token(&(optics_window_create_activation_token_opts){
                  .out_buf = tok_buf, .out_cap = 0}) != 0);
    OT_EXPECT(optics_window_create_activation_token(&(optics_window_create_activation_token_opts){
                  .out_buf = nullptr, .out_cap = sizeof tok_buf}) != 0);
    OT_EXPECT(optics_window_create_activation_token(&(optics_window_create_activation_token_opts){
                  .app_id = "org.example.App",
                  .out_buf = tok_buf,
                  .out_cap = sizeof tok_buf,
              }) != 0);

    return OT_REPORT();
}
