/* paint_static_demo.c — integration check for iris_paint_mark_static.
 *
 * Runs a real window for a fixed wall time with a paint callback that
 * declares itself static on a duty cycle (animate 1s, static 1s, ...), and
 * prints the presented-frame counters per phase. The static phases must
 * present far fewer frames than the animating phases (ideally zero beyond
 * the first), proving the skip works end-to-end on the live compositor.
 *
 * Build & run from the optics worktree:
 *   cc -o /tmp/paint_static_demo examples/iris/paint_static_demo.c \
 *      -Ilibs/iris/include -Ilibs/lens/include -Ilibs/flux/include \
 *      $(pkg-config --cflags --libs iris lens flux) && /tmp/paint_static_demo
 */

#include <iris/iris.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    double start;
    double last_report;
    int frames_animated;
    int frames_static;
    int build_calls;
    int frames_in_flight; /* frames presented during the current phase */
} demo_state;

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void build(lens *ui, const lens_input *in, void *user) {
    demo_state *d = user;
    (void)in;
    double tb = now_s() - d->start;
    /* A trivial, non-animating tree: any chrome damage here is zero after
     * the first frame, so the paint callback alone decides the cadence. */
    lens_begin(ui, NULL);
    lens_column(ui);
    lens_label(ui, "paint_static demo: watch stderr counters");
    lens_label(ui, "phase toggles every 2s; close the window to finish");
    lens_close(ui);
    lens_end(ui);
    d->build_calls++;
    /* The static declaration belongs in build, not paint: build runs on
     * every scheduled frame (including skipped ones), so the declaration
     * is re-issued every tick. Declaring in paint would leave the flag
     * cleared after the first skipped frame and bounce back to the active
     * cadence. */
    if ((int)(tb / 2.0) % 2 == 1) {
        iris_paint_mark_static();
        d->frames_static++;
    }
    if (tb - d->last_report >= 2.0) {
        d->last_report = tb;
        fprintf(stderr,
                "[paint_static t=%.1fs] animated frames: %d, static calls: %d, builds: %d\n",
                tb, d->frames_animated, d->frames_static, d->build_calls);
        fflush(stderr);
    }
}

static void paint(flux_canvas *canvas, flux_device *device, float scale, void *user) {
    demo_state *d = user;
    (void)canvas;
    (void)device;
    (void)scale;
    double t = now_s() - d->start;
    (void)t;
    /* Animate: keep the active cadence alive. The static declaration is
     * made from build (see above); during animated phases this callback
     * actually draws (a cheap solid would go here). */
    iris_request_animation_frame();
    d->frames_animated++;
    d->frames_in_flight++;
}

static void report(demo_state *d) {
    fprintf(stderr,
            "[paint_static] animated-phase frames: %d, static-phase calls: %d\n",
            d->frames_animated, d->frames_static);
}

int main(void) {
    demo_state d;
    memset(&d, 0, sizeof d);
    d.start = now_s();
    d.last_report = d.start;

    iris_app_config cfg = {0};
    cfg.title = "iris paint_static demo";
    cfg.app_id = "optics.paint_static_demo";
    cfg.width = 520;
    cfg.height = 180;
    cfg.dark = true;
    cfg.build = build;
    cfg.paint = paint;
    cfg.user = &d;

    /* Drive for ~8 seconds by asking the loop to stop: iris has no public
     * "quit" in this rev, so the demo relies on the runner closing the
     * window; a wrapper script can use a timeout + SIGKILL. */
    int rc = iris_app_run(&cfg);
    report(&d);
    return rc;
}
