/* test_drawlist_hash.c — hash_cmd must cover every pixel-affecting field.
 *
 * subtree_changed is driven by cmd_hash (ADR-0030 damage tracking).
 * hash_cmd used to skip flags (e.g. connected-tab shoulders),
 * text_weight, and the image pointer, so a frame that changed only one
 * of those produced an identical hash and the subtree was wrongly
 * reported unchanged.
 *
 * drawlist.c is compiled into the test binary directly (its symbols
 * are hidden in liblens) so the test can drive lensi_drawlist_push
 * onto a live retained node and read subtree_changed after lens_end.
 */

#include "../../libs/lens/src/internal.h"
#include "lens_test.h"
#include "optics_test.h"
#include <lens/lens.h>

static const lens_input IN0 = {.display_size = {200, 100}, .dt_seconds = 0.016f};

static lens_draw_cmd base_cmd(void) {
    return (lens_draw_cmd){
        .kind = LENS_DRAW_IMAGE,
        .rel = {4.0f, 4.0f, 16.0f, 16.0f},
        .color = 0xff112233u,
        .flags = 0u,
        .image = (flux_image *)(uintptr_t)0x1000u, /* hashed, never dereferenced */
    };
}

/* Run one frame pushing `cmd` onto the root, return its damage flag. */
static bool frame_with_cmd(lens *ui, lens_draw_cmd cmd) {
    lens_begin(ui, &IN0);
    lensi_drawlist_push(ui, lens_root(ui), cmd);
    lens_end(ui);
    lens_node *root = lens_root(ui);
    bool changed = root->subtree_changed;
    /* This unit isolates command hashing and deliberately does not invoke
     * snapshot publication (the fake image pointer must never be dereferenced).
     * Acknowledge the geometry baseline exactly as a successful render would
     * so geometry damage does not mask the hash assertions. */
    root->render_rect = root->final_rect;
    root->has_render_rect = true;
    return changed;
}

int main(void) {
    lens *ui = nullptr;
    OT_EXPECT(lens_create(&(lens_desc){}, &ui) == FLUX_OK);

    /* frame 1: first geometry — always changed */
    OT_EXPECT(frame_with_cmd(ui, base_cmd()) == true);
    /* frame 2: identical command — hash stable, subtree unchanged */
    OT_EXPECT(frame_with_cmd(ui, base_cmd()) == false);

    /* flags-only change must mark the subtree changed… */
    lens_draw_cmd cmd = base_cmd();
    cmd.flags = LENSI_TAB_CONNECT_LEFT | LENSI_TAB_CONNECT_RIGHT;
    OT_EXPECT(frame_with_cmd(ui, cmd) == true);
    /* …and settle back once the change is one frame old */
    OT_EXPECT(frame_with_cmd(ui, cmd) == false);

    /* image-pointer-only change */
    cmd = base_cmd();
    cmd.image = (flux_image *)(uintptr_t)0x2000u;
    OT_EXPECT(frame_with_cmd(ui, cmd) == true);
    OT_EXPECT(frame_with_cmd(ui, cmd) == false);

    /* text_weight-only change on a text command */
    lens_draw_cmd text = {
        .kind = LENS_DRAW_TEXT,
        .rel = {0.0f, 0.0f, 40.0f, 12.0f},
        .color = 0xffffffffu,
        .text = "label",
        .text_size = 14.0f,
        .text_weight = 400.0f,
    };
    OT_EXPECT(frame_with_cmd(ui, text) == true);  /* kind/image change too */
    OT_EXPECT(frame_with_cmd(ui, text) == false); /* identical → unchanged */
    text.text_weight = 700.0f;
    OT_EXPECT(frame_with_cmd(ui, text) == true); /* weight-only → changed */
    OT_EXPECT(frame_with_cmd(ui, text) == false);

    /* Foreground-contour changes must invalidate recorded pixels too. */
    text.outline_color = 0x80000000u;
    OT_EXPECT(frame_with_cmd(ui, text) == true);
    OT_EXPECT(frame_with_cmd(ui, text) == false);
    text.outline_width = 0.75f;
    OT_EXPECT(frame_with_cmd(ui, text) == true);
    OT_EXPECT(frame_with_cmd(ui, text) == false);

    lens_release(ui);
    return OT_REPORT();
}
