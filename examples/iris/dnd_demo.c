/*
 * dnd_demo.c — Interactive Cross-Platform Drag-and-Drop Demonstration.
 *
 * Demonstrates:
 *   1. Dragging items from Optics to external applications (VS Code, terminal, etc.)
 *   2. Dropping text and files from external applications into Optics drop targets
 *   3. Real-time visual feedback, hover states, and payload extraction.
 */

#include "app_shell.h"
#include <iris/app.h>
#include <iris/capability.h>
#include <iris/dnd.h>
#include <iris/window.h>
#include <lens/lens.h>

#include <stdio.h>
#include <string.h>

typedef struct app {
    char last_dropped_payload[1024];
    uint32_t dropped_len;
    uint32_t drop_count;
    bool target_hovered;
} app;

static void build(lens *ui, const lens_input *in, void *user) {
    app *a = user;
    lens_theme th = lens_get_theme(ui);
    shell_tones tn = shell_tones_from(&th);

    /* Esc quits */
    for (uint32_t k = 0; k < in->key_count; k++) {
        if (in->keys[k].pressed && in->keys[k].key == LENS_KEY_ESCAPE)
            iris_window_close();
    }

    lens_flex(ui, 1.0f);
    lens_column_begin(
        ui, &(lens_layout_opts){.pad = 24, .gap = 16, .cross = LENS_STRETCH, .bg = tn.card});

    /* Header */
    lens_size(ui, 0, 32);
    lens_label(ui, &(lens_label_opts){.text = "Cross-Platform Drag-and-Drop Subsystem (ADR-0086)",
                                      .size = 20.0f});

    lens_size(ui, 0, 20);
    char cap_str[128];
    snprintf(cap_str, sizeof(cap_str), "Capabilities: Drag Source: %s  |  Drop Target: %s",
             iris_supports(IRIS_CAP_DRAG_SOURCE) ? "Supported" : "No",
             iris_supports(IRIS_CAP_DROP_TARGET) ? "Supported" : "No");
    lens_label(ui, &(lens_label_opts){.text = cap_str});

    /* Split Row: Left = Drag Sources, Right = Drop Targets */
    lens_flex(ui, 1.0f);
    lens_row_begin(ui, &(lens_layout_opts){.gap = 20, .cross = LENS_STRETCH});

    /* --- Left Column: Drag Source Affordances --- */
    lens_flex(ui, 1.0f);
    lens_column_begin(
        ui, &(lens_layout_opts){.pad = 16, .gap = 12, .cross = LENS_STRETCH, .bg = tn.sidebar});

    lens_size(ui, 0, 24);
    lens_label(ui, &(lens_label_opts){.text = "1. Drag Out to External Apps", .size = 16.0f});
    lens_size(ui, 0, 18);
    lens_label(ui,
               &(lens_label_opts){.text = "Click and drag any card out to an editor or terminal:"});

    /* Draggable Item 1 */
    lens_id drag_id1 = lens_current_id(ui, "card1");
    lens_size(ui, 0, 60);
    lens_row_begin(ui,
                   &(lens_layout_opts){.pad = 12, .gap = 10, .cross = LENS_CENTER, .bg = tn.card});
    lens_label(ui, &(lens_label_opts){.text = "[Drag Me] Optics C23 Graphics Framework"});
    lens_close(ui);

    lens_dnd_source_desc sdesc1 = {
        .id = drag_id1,
        .text = "Optics: Unified C23 graphics and UI stack with Vulkan 1.3 engine",
        .text_len = strlen("Optics: Unified C23 graphics and UI stack with Vulkan 1.3 engine"),
        .actions = IRIS_DND_ACTION_COPY,
    };
    lens_dnd_source(ui, &sdesc1);

    /* Draggable Item 2 */
    lens_id drag_id2 = lens_current_id(ui, "card2");
    lens_size(ui, 0, 60);
    lens_row_begin(ui,
                   &(lens_layout_opts){.pad = 12, .gap = 10, .cross = LENS_CENTER, .bg = tn.card});
    lens_label(ui, &(lens_label_opts){.text = "[Drag Me] Code Snippet: lens_dnd_source()"});
    lens_close(ui);

    lens_dnd_source_desc sdesc2 = {
        .id = drag_id2,
        .text = "bool dragged = lens_dnd_source(ui, &desc);",
        .text_len = strlen("bool dragged = lens_dnd_source(ui, &desc);"),
        .actions = IRIS_DND_ACTION_COPY,
    };
    lens_dnd_source(ui, &sdesc2);

    lens_close(ui); /* End Left Column */

    /* --- Right Column: Drop Target Zone --- */
    lens_flex(ui, 1.0f);
    lens_column_begin(
        ui, &(lens_layout_opts){.pad = 16, .gap = 12, .cross = LENS_STRETCH, .bg = tn.sidebar});

    lens_size(ui, 0, 24);
    lens_label(ui, &(lens_label_opts){.text = "2. Drop In from External Apps", .size = 16.0f});
    lens_size(ui, 0, 18);
    lens_label(ui, &(lens_label_opts){.text = "Drag files or text from your OS and drop below:"});

    /* Drop Zone Target Box */
    lens_id drop_zone_id = lens_current_id(ui, "drop_zone");
    lens_flex(ui, 1.0f);

    uint32_t zone_bg = a->target_hovered ? th.color_accent : tn.card;
    lens_column_begin(
        ui, &(lens_layout_opts){.pad = 16, .gap = 8, .cross = LENS_STRETCH, .bg = zone_bg});

    lens_dnd_drop_info dinfo = {0};
    bool in_target = lens_dnd_drop_target(ui, drop_zone_id, IRIS_DND_ACTION_COPY, &dinfo);
    a->target_hovered = in_target && dinfo.is_hovered;

    if (dinfo.is_dropped) {
        char buf[1024] = {0};
        uint32_t n = lens_take_drop(ui, buf, sizeof(buf));
        if (n > 0) {
            memcpy(a->last_dropped_payload, buf, n);
            a->last_dropped_payload[n] = '\0';
            a->dropped_len = n;
            a->drop_count++;
        }
    }

    if (a->dropped_len > 0) {
        lens_size(ui, 0, 24);
        char summary[128];
        snprintf(summary, sizeof(summary), "Received Drop #%u (%u bytes):", a->drop_count,
                 a->dropped_len);
        lens_label(ui, &(lens_label_opts){.text = summary});

        lens_flex(ui, 1.0f);
        lens_size(ui, 0, 80);
        lens_label(ui, &(lens_label_opts){.text = a->last_dropped_payload, .size = 13.0f});
    } else {
        lens_size(ui, 0, 30);
        lens_label(ui, &(lens_label_opts){.text = "[ Drop Files or Text Here ]"});
    }

    lens_close(ui); /* End Drop Zone */
    lens_close(ui); /* End Right Column */

    lens_close(ui); /* End Split Row */
    lens_close(ui); /* End Root Column */
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    app a = {0};
    iris_app_config cfg = {
        .title = "Optics — Drag and Drop Demonstration",
        .width = 900,
        .height = 540,
        .user = &a,
        .build = build,
    };
    return iris_app_run(&cfg);
}
