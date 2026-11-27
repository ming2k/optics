/* test_icon_runtime.c — per-context vector icon registration & widget integration. CPU-only. */

#include "../../libs/ui/src/internal.h"
#include "optics_test.h"
#include "ui_test.h"
#include <optics/ui.h>

static const optics_ui_input IN0 = {.display_size = {200, 100}, .dt_seconds = 0.016f};

static const optics_ui_icon_cmd STROKE_CMDS[] = {
    {.kind = OPTICS_UI_ICON_CMD_CIRCLE, .params = {12.0f, 12.0f, 9.0f}},
    {.kind = OPTICS_UI_ICON_CMD_MOVE, .params = {12.0f, 7.0f}},
    {.kind = OPTICS_UI_ICON_CMD_LINE, .params = {12.0f, 13.0f}},
};

static const optics_ui_icon_cmd FILL_CMDS[] = {
    {.kind = OPTICS_UI_ICON_CMD_RECT, .params = {4.0f, 4.0f, 16.0f, 16.0f}},
};

static int has_icon_cmd(const optics_ui_node *n, optics_ui_icon_id id) {
    if (!n)
        return 0;
    for (uint32_t i = 0; i < n->cmd_count; i++)
        if (n->cmds[i].kind == OPTICS_UI_DRAW_ICON && n->cmds[i].icon_id == id)
            return 1;
    return 0;
}

int main(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    /* Validation: null, empty, out-of-range runs, and bad command kinds are rejected. */
    OT_EXPECT(
        optics_ui_icon_register(nullptr, &(optics_ui_icon_desc){.cmds = FILL_CMDS, .count = 1u}) ==
        OPTICS_UI_ICON_NONE);
    OT_EXPECT(optics_ui_icon_register(ui, nullptr) == OPTICS_UI_ICON_NONE);
    OT_EXPECT(optics_ui_icon_register(ui, &(optics_ui_icon_desc){}) == OPTICS_UI_ICON_NONE);
    OT_EXPECT(optics_ui_icon_register(ui, &(optics_ui_icon_desc){.cmds = FILL_CMDS, .count = 0u}) ==
              OPTICS_UI_ICON_NONE);
    const optics_ui_icon_run bad_run = {.first_cmd = 0u, .count = 2u};
    OT_EXPECT(optics_ui_icon_register(ui, &(optics_ui_icon_desc){
                                              .cmds = FILL_CMDS,
                                              .count = 1u,
                                              .runs = &bad_run,
                                              .run_count = 1u,
                                          }) == OPTICS_UI_ICON_NONE);

    optics_ui_icon_id stroke = optics_ui_icon_register(ui, &(optics_ui_icon_desc){
                                                               .cmds = STROKE_CMDS,
                                                               .count = 3u,
                                                               .paint = OPTICS_UI_ICON_PAINT_STROKE,
                                                           });
    optics_ui_icon_id fill = optics_ui_icon_register(ui, &(optics_ui_icon_desc){
                                                             .cmds = FILL_CMDS,
                                                             .count = 1u,
                                                             .paint = OPTICS_UI_ICON_PAINT_FILL,
                                                         });
    OT_EXPECT(stroke != OPTICS_UI_ICON_NONE);
    OT_EXPECT(fill != OPTICS_UI_ICON_NONE);
    OT_EXPECT(fill != stroke);

    /* Runtime ids work through every icon widget, alongside a built-in chrome chevron. */
    optics_ui_begin(ui, &IN0);
    optics_ui_icon(ui, &(optics_ui_icon_opts){.id = stroke, .size = 16.0f});
    optics_ui_icon(ui, &(optics_ui_icon_opts){.id = OPTICS_UI_ICON_CHEVRON_RIGHT, .size = 16.0f});
    optics_ui_button(ui, &(optics_ui_button_opts){.icon = fill});
    optics_ui_selectable(
        ui, &(optics_ui_selectable_opts){.icon = stroke, .label = "row", .selected = false});
    optics_ui_test_end(ui);
    OT_EXPECT(!optics_ui_overflowed(ui));

    optics_ui_node *child = optics_ui_node_first_child(optics_ui_root(ui));
    OT_EXPECT(has_icon_cmd(child, stroke));
    child = optics_ui_node_next_sibling(child);
    OT_EXPECT(has_icon_cmd(child, OPTICS_UI_ICON_CHEVRON_RIGHT));
    child = optics_ui_node_next_sibling(child);
    OT_EXPECT(has_icon_cmd(child, fill));
    child = optics_ui_node_next_sibling(child);
    OT_EXPECT(has_icon_cmd(child, stroke));

    /* Zero-initialized selectable stays icon-less (OPTICS_UI_ICON_NONE == 0). */
    optics_ui_begin(ui, &IN0);
    optics_ui_selectable(ui, &(optics_ui_selectable_opts){.label = "plain", .selected = false});
    optics_ui_test_end(ui);
    optics_ui_node *row = optics_ui_node_first_child(optics_ui_root(ui));
    OT_EXPECT(row && row->cmd_count == 1 && row->cmds[0].kind == OPTICS_UI_DRAW_TEXT);

    /* Unknown and unregistered ids draw nothing and do not crash. */
    optics_ui_icon_unregister(ui, fill);
    optics_ui_begin(ui, &IN0);
    optics_ui_icon(ui, &(optics_ui_icon_opts){.id = OPTICS_UI_ICON_NONE, .size = 16.0f});
    optics_ui_icon(ui, &(optics_ui_icon_opts){.id = fill, .size = 16.0f});
    optics_ui_test_end(ui);
    OT_EXPECT(!optics_ui_overflowed(ui));
    OT_EXPECT(optics_ui_node_first_child(optics_ui_root(ui)) == nullptr);

    /* Flood the per-frame arena */
    optics_ui_begin(ui, &IN0);
    for (int i = 0; i < 8000; i++)
        optics_ui_icon(ui, &(optics_ui_icon_opts){.id = stroke, .size = 16.0f});
    optics_ui_test_end(ui);
    OT_EXPECT(optics_ui_overflowed(ui));

    optics_ui_release(ui);

    /* Growing the registry must hand out clean slots. The headless allocator is
     * libc malloc, which does not zero (ASan fills fresh blocks with 0xbe), so
     * every slot past the registered ones must read unoccupied, generation 0,
     * and own no buffers, or teardown frees garbage pointers. 40 registrations
     * grow the table twice (16 -> 32 -> 64). */
    optics_ui *grow = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &grow) == OPTICS_GFX_OK);
    enum { GROW_COUNT = 40 };
    optics_ui_icon_id grown[GROW_COUNT] = {};
    for (uint32_t i = 0; i < GROW_COUNT; i++) {
        grown[i] = optics_ui_icon_register(grow, &(optics_ui_icon_desc){
                                                     .cmds = FILL_CMDS,
                                                     .count = 1u,
                                                     .paint = OPTICS_UI_ICON_PAINT_FILL,
                                                 });
        OT_EXPECT(grown[i] != OPTICS_UI_ICON_NONE);
        OT_EXPECT(optics_ui_icon_info(grow, grown[i]) != nullptr);
        for (uint32_t j = 0; j < i; j++)
            OT_EXPECT(grown[j] != grown[i]);
    }
    OT_EXPECT(grow->icons.cap >= GROW_COUNT);
    for (uint32_t i = GROW_COUNT; i < grow->icons.cap; i++) {
        const optics_ui__icon_slot *slot = &grow->icons.slots[i];
        OT_EXPECT(!slot->occupied);
        OT_EXPECT(slot->generation == 0u);
        OT_EXPECT(slot->cmds == nullptr && slot->runs == nullptr);
    }
    optics_ui_release(grow);
    return OT_REPORT();
}
