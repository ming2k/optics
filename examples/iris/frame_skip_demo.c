/* frame_skip_demo.c — integration check for iris_request_frame_skip_render.
 *
 * This is the runtime acceptance test for the ADR-0122 regression: a host
 * that streams media-cadence content by declaring zero-render skips on the
 * in-between frames used to drive the frame loop's scheduling baseline into
 * the past. `next_deadline = last_render_ns + ACTIVE_PERIOD_NS` was computed
 * against the timestamp of the last frame that actually painted; a run of
 * declared skips left that timestamp frozen, the poll budget went to zero,
 * and the loop busy-spun a full CPU core — a "skip" that cost 100% CPU.
 *
 * What to look for in the counters (every 2s on stderr):
 *
 *   - `ticks` should track the display refresh rate (~120/s here), NOT
 *     hundreds of thousands per second. A regression shows up as an absurd
 *     tick count and a pegged core while the window sits still.
 *   - `paints` / `skips` should sit at roughly the requested duty cycle
 *     (this demo asks for 1 paint in 4), so the GPU work really is skipped.
 *
 * Measure CPU alongside it:
 *   /usr/bin/time -v ./frame_skip_demo     (or: top -p $(pgrep frame_skip_demo))
 *
 * Build & run from the optics worktree, or via `meson compile -C build`.
 */

#include <iris/iris.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* One painted frame in four; the other three are declared skips. That is the
 * shape a 30 fps visualiser has on a 120 Hz panel. */
#define PAINT_EVERY 4

typedef struct {
    double start;
    double last_report;
    int tick;   /* build invocations = frame-loop iterations */
    int paints; /* frames that reached the GPU                   */
    int skips;  /* frames the host declared zero-render          */
    int phase_paints;
    int phase_skips;
    int phase_ticks;
} skip_state;

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void build(lens *ui, const lens_input *in, void *user) {
    skip_state *d = user;
    (void)in;
    double tb = now_s() - d->start;

    /* iris brackets this callback with lens_begin/lens_end: a build only
     * emits widgets. */
    lens_column_begin(ui, NULL);
    lens_label(ui, &(lens_label_opts){.text = "frame_skip demo: watch stderr counters"});
    lens_label(ui, &(lens_label_opts){.text = "one paint in four; close the window to finish"});
    lens_close(ui);

    d->tick++;
    d->phase_ticks++;

    /* Keep the active (display-locked) cadence alive: this is what used to
     * combine with the skip below to freeze the scheduling baseline. */
    iris_request_animation_frame();

    if ((d->tick % PAINT_EVERY) != 0) {
        /* The canvas content is unchanged this frame, but the cadence must
         * keep running at the display rate. */
        iris_request_frame_skip_render();
        d->skips++;
        d->phase_skips++;
    }

    if (tb - d->last_report >= 2.0) {
        double dt = tb - d->last_report;
        d->last_report = tb;
        fprintf(stderr,
                "[frame_skip t=%.1fs] ticks=%d (%.0f/s) paints=%d skips=%d "
                "| this window: ticks=%d (%.0f/s) paints=%d skips=%d\n",
                tb, d->tick, d->tick / tb, d->paints, d->skips, d->phase_ticks, d->phase_ticks / dt,
                d->phase_paints, d->phase_skips);
        fflush(stderr);
        d->phase_ticks = d->phase_paints = d->phase_skips = 0;
    }
}

static void paint(flux_canvas *canvas, flux_device *device, float scale, void *user) {
    skip_state *d = user;
    (void)canvas;
    (void)device;
    (void)scale;
    d->paints++;
    d->phase_paints++;
}

int main(void) {
    skip_state d;
    memset(&d, 0, sizeof d);
    d.start = now_s();
    /* Elapsed-seconds baseline: see paint_static_demo. */
    d.last_report = 0.0;

    return iris_app_run(&(iris_app_config){
        .title = "iris frame_skip demo",
        .app_id = "optics.frame_skip_demo",
        .width = 560,
        .height = 180,
        .dark = true,
        .build = build,
        .paint = paint,
        .user = &d,
    });
}
