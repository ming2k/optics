/* test_dnd.c — Drag-and-drop subsystem verification (ADR-0086). */

#include "lens_test.h"
#include "optics_test.h"
#include <optics/ui.h>
#include <string.h>

typedef struct host_state {
    int start_drag_calls;
    char last_drag_text[256];
    size_t last_drag_len;
    uint32_t last_actions;
} host_state;

static int host_start_drag(const char *text, size_t len, uint32_t actions, void *user) {
    host_state *s = user;
    s->start_drag_calls++;
    s->last_drag_len = len;
    s->last_actions = actions;
    if (text && len < sizeof(s->last_drag_text)) {
        memcpy(s->last_drag_text, text, len);
        s->last_drag_text[len] = '\0';
    }
    return 0;
}

int main(void) {
    host_state host = {};
    optics_ui_desc desc = OPTICS_UI_DESC_INIT;
    desc.dnd = (optics_ui_dnd_host){
        .start_drag = host_start_drag,
        .user = &host,
    };

    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&desc, &ui) == OPTICS_GFX_OK);
    OT_EXPECT(ui != nullptr);

    /* Frame 1: Build layout with two buttons */
    optics_ui_input in = {.display_size = {400, 400}, .dt_seconds = 0.016f};
    in.cursor = (optics_gfx_point){50, 50};
    optics_ui_begin(ui, &in);
    optics_ui_row_begin(ui, nullptr);
    optics_ui_id src_id = optics_ui_current_id(ui, "Source");
    optics_ui_button(
        ui, &(optics_ui_button_opts){.label = "Source", .box.width = 100, .box.height = 40});
    optics_ui_id dst_id = optics_ui_current_id(ui, "Target");
    optics_ui_button(
        ui, &(optics_ui_button_opts){.label = "Target", .box.width = 100, .box.height = 40});
    optics_ui_close(ui);
    optics_ui_test_end(ui);

    /* Frame 2: Mouse press down on source button (x=20, y=20) */
    in.cursor = (optics_gfx_point){20, 20};
    in.mouse_down[OPTICS_UI_MOUSE_LEFT] = true;
    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    optics_ui_row_begin(ui, nullptr);
    optics_ui_button(
        ui, &(optics_ui_button_opts){.label = "Source", .box.width = 100, .box.height = 40});

    optics_ui_dnd_source_desc sdesc = {
        .id = src_id,
        .text = "Hello Drag",
        .text_len = strlen("Hello Drag"),
        .actions = 1,
    };
    bool dragged = optics_ui_dnd_source(ui, &sdesc);
    OT_EXPECT(!dragged); /* Not dragged yet (moved distance 0 <= threshold) */
    OT_EXPECT(host.start_drag_calls == 0);

    optics_ui_button(
        ui, &(optics_ui_button_opts){.label = "Target", .box.width = 100, .box.height = 40});
    optics_ui_dnd_drop_info dinfo = {};
    bool is_target = optics_ui_dnd_drop_target(
        ui,
        &(optics_ui_dnd_drop_target_opts){.id = dst_id, .accepted_actions = 1, .out_info = &dinfo});
    OT_EXPECT(!is_target);
    OT_EXPECT(!dinfo.is_hovered);

    optics_ui_close(ui);
    optics_ui_test_end(ui);

    /* Frame 3: Pointer moves past drag threshold (x=20, y=40 -> dy = 20 > 4) */
    in.cursor = (optics_gfx_point){20, 40};
    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = false;
    in.mouse_down[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    optics_ui_row_begin(ui, nullptr);
    optics_ui_button(
        ui, &(optics_ui_button_opts){.label = "Source", .box.width = 100, .box.height = 40});

    dragged = optics_ui_dnd_source(ui, &sdesc);
    OT_EXPECT(dragged); /* Drag threshold exceeded! */
    OT_EXPECT(host.start_drag_calls == 1);
    OT_EXPECT(strcmp(host.last_drag_text, "Hello Drag") == 0);

    optics_ui_button(
        ui, &(optics_ui_button_opts){.label = "Target", .box.width = 100, .box.height = 40});
    optics_ui_close(ui);
    optics_ui_test_end(ui);

    /* Frame 4: Platform delivers drop payload onto destination target */
    optics_ui_node *tnode = optics_ui_find(ui, dst_id);
    OT_EXPECT(tnode != nullptr);
    optics_gfx_rect tb = optics_ui_node_bounds(tnode);

    optics_gfx_point drop_point = (optics_gfx_point){tb.x + 10, tb.y + 10};
    optics_ui_deliver_drop(ui, &(optics_ui_deliver_drop_opts){.payload = "Dropped Payload",
                                                              .len = strlen("Dropped Payload"),
                                                              .pos = drop_point});

    in.cursor = drop_point;
    in.mouse_down[OPTICS_UI_MOUSE_LEFT] = false;
    in.mouse_released[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    optics_ui_row_begin(ui, nullptr);
    optics_ui_button(
        ui, &(optics_ui_button_opts){.label = "Source", .box.width = 100, .box.height = 40});

    dragged = optics_ui_dnd_source(ui, &sdesc);
    OT_EXPECT(!dragged); /* Drag ended on mouse release */

    optics_ui_button(
        ui, &(optics_ui_button_opts){.label = "Target", .box.width = 100, .box.height = 40});
    is_target = optics_ui_dnd_drop_target(
        ui,
        &(optics_ui_dnd_drop_target_opts){.id = dst_id, .accepted_actions = 1, .out_info = &dinfo});
    OT_EXPECT(is_target);
    OT_EXPECT(dinfo.is_hovered);
    OT_EXPECT(dinfo.is_dropped);

    char buf[64] = {};
    uint32_t len =
        optics_ui_take_drop(ui, &(optics_ui_take_drop_opts){.dst = buf, .cap = sizeof(buf)});
    OT_EXPECT(len == strlen("Dropped Payload"));
    OT_EXPECT(strcmp(buf, "Dropped Payload") == 0);

    optics_ui_close(ui);
    optics_ui_test_end(ui);

    optics_ui_release(ui);
    return OT_REPORT();
}
