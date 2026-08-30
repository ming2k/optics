/* test_dnd.c — Drag-and-drop subsystem verification (ADR-0086). */

#include "test_helpers.h"
#include <lens/lens.h>
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
    host_state host = {0};
    lens_desc desc = LENS_DESC_INIT;
    desc.dnd = (lens_dnd_host){
        .start_drag = host_start_drag,
        .user = &host,
    };

    lens *ui = NULL;
    CHECK(lens_create(&desc, &ui) == FLUX_OK);
    CHECK(ui != NULL);

    /* Frame 1: Build layout with two buttons */
    lens_input in = {.display_size = {400, 400}, .dt_seconds = 0.016f};
    in.cursor = (flux_point){50, 50};
    lens_begin(ui, &in);
    lens_row_begin(ui, NULL);
    lens_id src_id = lens_current_id(ui, "Source");
    lens_button(ui, &(lens_button_opts){.label = "Source", .box.width = 100, .box.height = 40});
    lens_id dst_id = lens_current_id(ui, "Target");
    lens_button(ui, &(lens_button_opts){.label = "Target", .box.width = 100, .box.height = 40});
    lens_close(ui);
    lens_end(ui);

    /* Frame 2: Mouse press down on source button (x=20, y=20) */
    in.cursor = (flux_point){20, 20};
    in.mouse_down[LENS_MOUSE_LEFT] = true;
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    lens_row_begin(ui, NULL);
    lens_button(ui, &(lens_button_opts){.label = "Source", .box.width = 100, .box.height = 40});

    lens_dnd_source_desc sdesc = {
        .id = src_id,
        .text = "Hello Drag",
        .text_len = strlen("Hello Drag"),
        .actions = 1,
    };
    bool dragged = lens_dnd_source(ui, &sdesc);
    CHECK(!dragged); /* Not dragged yet (moved distance 0 <= threshold) */
    CHECK(host.start_drag_calls == 0);

    lens_button(ui, &(lens_button_opts){.label = "Target", .box.width = 100, .box.height = 40});
    lens_dnd_drop_info dinfo = {0};
    bool is_target = lens_dnd_drop_target(ui, dst_id, 1, &dinfo);
    CHECK(!is_target);
    CHECK(!dinfo.is_hovered);

    lens_close(ui);
    lens_end(ui);

    /* Frame 3: Pointer moves past drag threshold (x=20, y=40 -> dy = 20 > 4) */
    in.cursor = (flux_point){20, 40};
    in.mouse_pressed[LENS_MOUSE_LEFT] = false;
    in.mouse_down[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    lens_row_begin(ui, NULL);
    lens_button(ui, &(lens_button_opts){.label = "Source", .box.width = 100, .box.height = 40});

    dragged = lens_dnd_source(ui, &sdesc);
    CHECK(dragged); /* Drag threshold exceeded! */
    CHECK(host.start_drag_calls == 1);
    CHECK(strcmp(host.last_drag_text, "Hello Drag") == 0);

    lens_button(ui, &(lens_button_opts){.label = "Target", .box.width = 100, .box.height = 40});
    lens_close(ui);
    lens_end(ui);

    /* Frame 4: Platform delivers drop payload onto destination target */
    lens_node *tnode = lens_find(ui, dst_id);
    CHECK(tnode != NULL);
    flux_rect tb = lens_node_bounds(tnode);

    flux_point drop_point = (flux_point){tb.x + 10, tb.y + 10};
    lens_deliver_drop(ui, "Dropped Payload", strlen("Dropped Payload"), drop_point);

    in.cursor = drop_point;
    in.mouse_down[LENS_MOUSE_LEFT] = false;
    in.mouse_released[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    lens_row_begin(ui, NULL);
    lens_button(ui, &(lens_button_opts){.label = "Source", .box.width = 100, .box.height = 40});

    dragged = lens_dnd_source(ui, &sdesc);
    CHECK(!dragged); /* Drag ended on mouse release */

    lens_button(ui, &(lens_button_opts){.label = "Target", .box.width = 100, .box.height = 40});
    is_target = lens_dnd_drop_target(ui, dst_id, 1, &dinfo);
    CHECK(is_target);
    CHECK(dinfo.is_hovered);
    CHECK(dinfo.is_dropped);

    char buf[64] = {0};
    uint32_t len = lens_take_drop(ui, buf, sizeof(buf));
    CHECK(len == strlen("Dropped Payload"));
    CHECK(strcmp(buf, "Dropped Payload") == 0);

    lens_close(ui);
    lens_end(ui);

    lens_destroy(ui);
    return TEST_REPORT();
}
