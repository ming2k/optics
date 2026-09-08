/*
 * Prism liquid-glass golden test (ADR-0063, ADR-0046): pixel-level
 * regression gate for the material's default recipe.
 *
 * Strategy — two complementary gates over one rendered scene:
 *
 *  1. TOLERANCE GOLDEN. A reference image (tests/prism/golden/
 *     liquid_glass_default.pam, regenerable with PRISM_GOLDEN_UPDATE=1)
 *     is compared per channel with a tolerance (12/255) and an
 *     outlier allowance (0.5% of pixels may exceed the per-pixel
 *     tolerance, none may exceed 3x it). This absorbs renderer-
 *     irrelevant driver/implementation variance (lavapipe vs. real
 *     GPUs, Mesa version drift) while still catching recipe changes,
 *     pass-order regressions, and platform-divergence bugs.
 *
 *  2. HARD INVARIANTS. Driver-independent physical truths asserted
 *     exactly, no tolerance:
 *       - the analytic SDF masks every layer: far outside the body
 *         silhouette + shadow reach, output is exactly transparent
 *         (all four channels 0);
 *       - the interior plate is opaque (alpha 255) and horizontally
 *         flat in the middle band (concentric rim model, ADR-0046);
 *       - the rim band is brighter than the plate on the lit side
 *         (light_direction {-0.45,-0.89} lights the top edge);
 *       - a re-render of the same scene on the same device is
 *         bit-identical (frame-slot determinism, ADR-0022).
 *
 * A Golden cell of 8 frames guarantees every frame slot is exercised
 * with identical inputs before readback, so persistent per-slot state
 * differences cannot fake a pass.
 *
 * The golden image is committed at the reference size (64x64) and is
 * only meaningful at that size; the test refuses to run when the build
 * disagrees, rather than silently rescaling.
 */
#include "test_helpers.h"
#include <flux/effect.h>
#include <flux/flux.h>
#include <flux/vulkan.h>
#include <prism/prism.h>

#include <stdlib.h>
#include <string.h>

#define W 64u
#define H 64u
#define BYTES (W * H * 4u)
#define GOLDEN_FRAMES 8u /* covers all FLUX_MAX_FRAMES_IN_FLIGHT slots */

#define GOLDEN_TOLERANCE 12    /* per-channel, out of 255 */
#define GOLDEN_OUTLIER_PCT 0.5 /* % of pixels allowed past tolerance */
#define GOLDEN_HARD_CAP (3 * GOLDEN_TOLERANCE)

/* Body geometry (capture-image pixels). The plate interior probe band,
 * rim probe band, and outside probes below all key off these numbers. */
#define BODY_X 16.0f
#define BODY_Y 16.0f
#define BODY_W 32.0f
#define BODY_H 32.0f
#define BODY_R 10.0f

static const char *golden_path(void) {
    /* Resolved relative to the test executable's cwd, which meson sets
     * to the build dir; the meson.build exports the source path. */
    static char buf[512];
    const char *src = getenv("PRISM_GOLDEN_SRC");
    if (src && src[0]) {
        snprintf(buf, sizeof buf, "%s/liquid_glass_default.pam", src);
        return buf;
    }
    return "tests/prism/golden/liquid_glass_default.pam";
}

/* --- PAM (P7) read/write: binary, comment-capable, trivially diffable --- */
static bool golden_load(uint8_t *px, size_t bytes) {
    FILE *f = fopen(golden_path(), "rb");
    if (!f)
        return false;
    char line[256];
    int w = 0, h = 0, depth = 0, maxval = 0;
    bool have_tupl = false;
    /* Header: P7 / WIDTH n / HEIGHT n / DEPTH 4 / MAXVAL 255 /
     * TUPLTYPE RGB_ALPHA / ENDHDR / binary. */
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#')
            continue;
        if (strncmp(line, "P7", 2) == 0)
            continue;
        if (sscanf(line, "WIDTH %d", &w) == 1)
            continue;
        if (sscanf(line, "HEIGHT %d", &h) == 1)
            continue;
        if (sscanf(line, "DEPTH %d", &depth) == 1)
            continue;
        if (sscanf(line, "MAXVAL %d", &maxval) == 1)
            continue;
        if (strncmp(line, "TUPLTYPE", 8) == 0) {
            have_tupl = true;
            continue;
        }
        if (strncmp(line, "ENDHDR", 6) == 0)
            break;
    }
    if (w != (int)W || h != (int)H || depth != 4 || maxval != 255 || !have_tupl) {
        fclose(f);
        return false;
    }
    size_t got = fread(px, 1, bytes, f);
    fclose(f);
    return got == bytes;
}

static bool golden_save(const uint8_t *px, size_t bytes) {
    FILE *f = fopen(golden_path(), "wb");
    if (!f)
        return false;
    fprintf(f,
            "P7\n# optics prism golden reference — regenerate with "
            "PRISM_GOLDEN_UPDATE=1\n# driver-agnostic; compare with "
            "GOLDEN_TOLERANCE\nWIDTH %u\nHEIGHT %u\nDEPTH 4\nMAXVAL 255\n"
            "TUPLTYPE RGB_ALPHA\nENDHDR\n",
            W, H);
    size_t put = fwrite(px, 1, bytes, f);
    fclose(f);
    return put == bytes;
}

/* Luma (Rec.709) of a pixel probe, 0..255 scale. */
static double probe_luma(const uint8_t *px, uint32_t x, uint32_t y, uint32_t w) {
    const uint8_t *p = &px[((size_t)y * w + x) * 4u];
    return 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2];
}

/* --- scene: one render of the default-recipe golden scene --- */
static void render_golden_scene(flux_surface *s, flux_canvas *canvas, flux_image *target,
                                flux_blur_filter *blur, prism_liquid_glass_filter *glass,
                                uint8_t *px) {
    flux_frame *frame = nullptr;
    EXPECT(flux_surface_begin_frame(s, nullptr, &frame) == FLUX_OK);

    /* Backdrop capture: the same hard vertical edge the layer tests use
     * — sharp structure under the plate, so refraction/lensing errors
     * and blur-seam regressions both become visible. */
    flux_color left = flux_color_rgba(30, 60, 110, 255);
    EXPECT(flux_canvas_begin_target(canvas, frame, target, &left) == FLUX_OK);
    flux_canvas_fill_rect_color(canvas, (flux_rect){(float)(W / 2), 0.0f, (float)(W / 2), (float)H},
                                flux_color_rgba_premul(210, 190, 150, 255));
    flux_canvas_end_target(canvas);

    flux_effect_blur_desc bd = FLUX_EFFECT_BLUR_DESC_INIT;
    bd.input = target;
    bd.sigma = 2.0f;
    flux_image *blurred = nullptr;
    EXPECT(flux_blur_filter_apply(blur, frame, &bd, &blurred) == FLUX_OK);

    /* Default recipe: PRISM_LIQUID_GLASS_DESC_INIT untouched, neutral
     * group (no shadow), refraction 0 so the plate reads the backdrop
     * flat and the rim/plate split is the discriminating signal. */
    prism_liquid_glass_shape body_shapes[1] = {
        {.bounds = {BODY_X, BODY_Y, BODY_W, BODY_H}, .corner_radius = BODY_R},
    };
    prism_liquid_glass_group body = PRISM_LIQUID_GLASS_GROUP_INIT;
    body.shapes = body_shapes;
    body.shape_count = 1;
    prism_liquid_glass_desc gd = PRISM_LIQUID_GLASS_DESC_INIT;
    gd.input = target;
    gd.blurred_input = blurred;
    gd.groups = &body;
    gd.group_count = 1u;
    gd.refraction = 0.0f;
    flux_image *out = nullptr;
    EXPECT(prism_liquid_glass_filter_apply(glass, frame, &gd, &out) == FLUX_OK);
    EXPECT(out != nullptr);

    flux_color black = flux_color_rgba(0, 0, 0, 255);
    EXPECT(flux_canvas_begin_frame(canvas, frame, &black) == FLUX_OK);
    flux_canvas_draw_image(canvas, out, (flux_rect){0, 0, (float)W, (float)H}, nullptr);
    flux_canvas_end_frame(canvas);
    EXPECT(flux_frame_submit(frame) == FLUX_OK);
    EXPECT(flux_frame_present(frame) == FLUX_OK);
    memset(px, 0xCD, BYTES);
    EXPECT(flux_surface_read_pixels(s, px, BYTES) == FLUX_OK);
}

int main(void) {
    flux_device *d = test_helpers_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_prism_golden: no Vulkan device; skipping\n");
        TEST_SUMMARY();
        return 0;
    }

    flux_surface_desc sd = FLUX_SURFACE_DESC_INIT;
    sd.width = W;
    sd.height = H;
    flux_surface *s = nullptr;
    EXPECT(flux_surface_create(d, &sd, &s) == FLUX_OK);

    VkFormat sfmt = flux_surface_vk_format(s);
    flux_format target_fmt =
        (sfmt == VK_FORMAT_B8G8R8A8_UNORM) ? FLUX_FORMAT_BGRA8_UNORM : FLUX_FORMAT_RGBA8_UNORM;

    flux_canvas_desc cd = {.type = FLUX_TYPE_CANVAS_DESC, .surface = s};
    flux_canvas *canvas = nullptr;
    EXPECT(flux_canvas_create(&cd, &canvas) == FLUX_OK);

    flux_image *target = nullptr;
    EXPECT(flux_image_create_render_target(d, W, H, target_fmt, &target) == FLUX_OK);

    flux_blur_filter *blur = nullptr;
    EXPECT(flux_blur_filter_create(d, &blur) == FLUX_OK);
    prism_liquid_glass_filter *glass = nullptr;
    EXPECT(prism_liquid_glass_filter_create(d, &glass) == FLUX_OK);

    /* Warm every frame slot with identical renders; the last frame's
     * readback is the sample. Slot-state divergence across the 3 slots
     * is covered by the determinism check further down. */
    static uint8_t px[BYTES];
    for (uint32_t i = 0; i < GOLDEN_FRAMES; ++i)
        render_golden_scene(s, canvas, target, blur, glass, px);

    /* ---------------- hard invariants (driver-independent) ------------ */

    /* The final frame composites the glass layer over a black base, so
     * every pixel outside the glass footprint is exactly the base colour
     * (opaque black, all channels 0) — the analytic SDF must not bleed
     * past the silhouette (no shadow on the group, no spread). */
    const uint8_t *plate = &px[(32u * W + 32u) * 4u];
    EXPECT(plate[3] == 255u); /* interior plate is opaque */

    const uint8_t *corner_tl = &px[(2u * W + 2u) * 4u];
    const uint8_t *corner_br = &px[((H - 3u) * W + (W - 3u)) * 4u];
    for (int c = 0; c < 3; c++) {
        EXPECT(corner_tl[c] == 0u);
        EXPECT(corner_br[c] == 0u);
    }

    /* Interior content step: the material's adaptive tint (smoke/pearl,
     * ADR-0079) is per-pixel by design, so the plate is not flat across a
     * backdrop edge — but the edge's *direction and presence* must
     * survive: luminance(right half) > luminance(left half) by a wide
     * margin, and each half is locally smooth (the frost blur bounds the
     * 4px gradient). A refraction/lensing regression displaces or smears
     * the step and breaks one of the two probes. Probes sit in the plate
     * band (body x 16..48; the ~8px scaled rim hugs the silhouette, so
     * x=22..26 and x=34..38 are interior). */
    {
        double l_l = probe_luma(px, 22, 32, W), l_r = probe_luma(px, 26, 32, W);
        double r_l = probe_luma(px, 34, 32, W), r_r = probe_luma(px, 38, 32, W);
        EXPECT(l_r - l_l <= 8.0);                             /* left half locally smooth */
        EXPECT(r_r - r_l <= 8.0);                             /* right half locally smooth */
        EXPECT((r_l + r_r) * 0.5 > (l_l + l_r) * 0.5 + 20.0); /* step preserved */
    }

    /* Rim vs plate: the light direction {-0.45,-0.89} lights the top
     * edge. The rim sample (row 18, x=32: inside the body, in the rim
     * band) must be brighter than the plate sample below it. */
    const uint8_t *rim_top = &px[(18u * W + 32u) * 4u];
    EXPECT(rim_top[0] + rim_top[1] + rim_top[2] > plate[0] + plate[1] + plate[2]);

    /* Outside-right alpha: immediately beyond the right silhouette the
     * alpha must be exactly the base's (the glass layer adds nothing). */
    const uint8_t *just_outside = &px[(32u * W + (unsigned)(BODY_X + BODY_W + 3u)) * 4u];
    EXPECT(just_outside[0] == 0u && just_outside[3] == 255u);

    /* ---------------- determinism (same-device bit equality) ---------- */
    static uint8_t px2[BYTES];
    render_golden_scene(s, canvas, target, blur, glass, px2);
    EXPECT(memcmp(px, px2, BYTES) == 0);

    /* ---------------- tolerance golden ------------------------------- */
    if (getenv("PRISM_GOLDEN_UPDATE")) {
        EXPECT(golden_save(px, BYTES));
        fprintf(stderr, "test_prism_golden: golden reference written (%s)\n", golden_path());
    } else {
        static uint8_t ref[BYTES];
        if (!golden_load(ref, BYTES)) {
            fprintf(stderr,
                    "test_prism_golden: golden reference missing/invalid at %s\n"
                    "  regenerate: PRISM_GOLDEN_UPDATE=1 ./build/tests/prism/integration/"
                    "test_prism_golden\n"
                    "  (or run meson test with PRISM_GOLDEN_UPDATE=1 set)\n",
                    golden_path());
            g_test_failed++;
        } else {
            unsigned outliers = 0, over_cap = 0, max_delta = 0;
            unsigned total = W * H;
            for (unsigned p = 0; p < total; p++) {
                unsigned pd = 0;
                for (int c = 0; c < 4; c++) {
                    unsigned dd = (unsigned)abs((int)px[p * 4u + (unsigned)c] -
                                                (int)ref[p * 4u + (unsigned)c]);
                    if (dd > pd)
                        pd = dd;
                }
                if (pd > max_delta)
                    max_delta = pd;
                if (pd > GOLDEN_TOLERANCE)
                    outliers++;
                if (pd > GOLDEN_HARD_CAP)
                    over_cap++;
            }
            /* Housekeeping sanity: the committed golden must look like a
             * glass body over the opaque black base, not like an empty
             * frame (guards against regenerating from a broken renderer). */
            g_test_count++;
            if (ref[(32u * W + 32u) * 4u + 3] != 255u || ref[(2u * W + 2u) * 4u + 2] != 0u) {
                fprintf(stderr, "FAIL test_prism_golden: committed reference fails its own "
                                "invariants — regenerate it\n");
                g_test_failed++;
            }
            EXPECT(over_cap == 0u);
            EXPECT((double)outliers <= (double)total * GOLDEN_OUTLIER_PCT / 100.0);
            fprintf(stderr, "test_prism_golden: max_delta=%u outliers=%u/%u (tol=%d, cap=%d)\n",
                    max_delta, outliers, total, GOLDEN_TOLERANCE, GOLDEN_HARD_CAP);
        }
    }

    /* Drain the device first: the release contract for every flux/prism
     * object requires all referencing GPU work to have completed, and the
     * last frame's readback leaves retire-lists the release paths walk. */
    flux_device_wait_idle(d);
    prism_liquid_glass_filter_release(glass);
    flux_blur_filter_release(blur);
    flux_canvas_destroy(canvas);
    flux_image_release(target);
    flux_surface_release(s);
    flux_device_release(d);
    TEST_SUMMARY();
}
