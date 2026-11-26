/* icon_runtime.c — built-in geometric chrome glyphs and per-context vector icon registry.
 *
 * Built-in icons are limited strictly to the four directional chevrons needed
 * by built-in patterns (tree_view disclosure, datagrid sort). All application
 * icons are registered per `optics_ui` context via `optics_ui_icon_register`,
 * using generational slot handles so unregistered ids and cached coverage
 * masks can never alias.
 */

#include "internal.h"
#include "render/icon_mask.h"

#include <string.h>

enum {
    OPTICS_UI_ICON_MAX_CMDS = 8192u,
    OPTICS_UI_ICON_MAX_REGISTERED = 4096u,
    OPTICS_UI_ICON_SLOT_MASK = 0xFFFFu,
    OPTICS_UI_ICON_GEN_SHIFT = 16u,
};

/* Minimal built-in geometric chrome glyphs (24x24 box, stroke mode). */
static const optics_ui_icon_cmd k_chevron_up_cmds[] = {
    {.kind = OPTICS_UI_ICON_CMD_MOVE, .params = {18.0f, 15.0f}},
    {.kind = OPTICS_UI_ICON_CMD_LINE, .params = {12.0f, 9.0f}},
    {.kind = OPTICS_UI_ICON_CMD_LINE, .params = {6.0f, 15.0f}},
};

static const optics_ui_icon_cmd k_chevron_down_cmds[] = {
    {.kind = OPTICS_UI_ICON_CMD_MOVE, .params = {6.0f, 9.0f}},
    {.kind = OPTICS_UI_ICON_CMD_LINE, .params = {12.0f, 15.0f}},
    {.kind = OPTICS_UI_ICON_CMD_LINE, .params = {18.0f, 9.0f}},
};

static const optics_ui_icon_cmd k_chevron_left_cmds[] = {
    {.kind = OPTICS_UI_ICON_CMD_MOVE, .params = {15.0f, 18.0f}},
    {.kind = OPTICS_UI_ICON_CMD_LINE, .params = {9.0f, 12.0f}},
    {.kind = OPTICS_UI_ICON_CMD_LINE, .params = {15.0f, 6.0f}},
};

static const optics_ui_icon_cmd k_chevron_right_cmds[] = {
    {.kind = OPTICS_UI_ICON_CMD_MOVE, .params = {9.0f, 18.0f}},
    {.kind = OPTICS_UI_ICON_CMD_LINE, .params = {15.0f, 12.0f}},
    {.kind = OPTICS_UI_ICON_CMD_LINE, .params = {9.0f, 6.0f}},
};

static const optics_ui_icon_desc k_builtin_icons[OPTICS_UI_ICON_BUILTIN_COUNT] = {
    [OPTICS_UI_ICON_NONE] = {},
    [OPTICS_UI_ICON_CHEVRON_UP] =
        {
            .cmds = k_chevron_up_cmds,
            .count = 3u,
            .paint = OPTICS_UI_ICON_PAINT_STROKE,
        },
    [OPTICS_UI_ICON_CHEVRON_DOWN] =
        {
            .cmds = k_chevron_down_cmds,
            .count = 3u,
            .paint = OPTICS_UI_ICON_PAINT_STROKE,
        },
    [OPTICS_UI_ICON_CHEVRON_LEFT] =
        {
            .cmds = k_chevron_left_cmds,
            .count = 3u,
            .paint = OPTICS_UI_ICON_PAINT_STROKE,
        },
    [OPTICS_UI_ICON_CHEVRON_RIGHT] =
        {
            .cmds = k_chevron_right_cmds,
            .count = 3u,
            .paint = OPTICS_UI_ICON_PAINT_STROKE,
        },
};

void optics_ui__icon_registry_init(optics_ui *ui) {
    if (!ui)
        return;
    ui->icons = (optics_ui__icon_registry){};
}

void optics_ui__icon_registry_destroy(optics_ui *ui) {
    if (!ui)
        return;
    for (uint32_t i = 0; i < ui->icons.cap; ++i) {
        optics_ui__icon_slot *slot = &ui->icons.slots[i];
        if (!slot->occupied)
            continue;
        optics_ui__free(ui, slot->cmds);
        optics_ui__free(ui, slot->runs);
        slot->cmds = nullptr;
        slot->runs = nullptr;
        slot->occupied = false;
    }
    optics_ui__free(ui, ui->icons.slots);
    ui->icons = (optics_ui__icon_registry){};
}

static bool decode_registered(const optics_ui *ui, optics_ui_icon_id id,
                              const optics_ui__icon_slot **out_slot) {
    if (!ui || !ui->icons.slots)
        return false;
    uint32_t raw = (uint32_t)id;
    uint32_t gen = raw >> OPTICS_UI_ICON_GEN_SHIFT;
    uint32_t idx = raw & OPTICS_UI_ICON_SLOT_MASK;
    if (gen == 0u || idx >= ui->icons.cap)
        return false;
    const optics_ui__icon_slot *slot = &ui->icons.slots[idx];
    if (!slot->occupied || slot->generation != gen)
        return false;
    if (out_slot)
        *out_slot = slot;
    return true;
}

bool optics_ui__icon_valid(const optics_ui *ui, optics_ui_icon_id id) {
    uint32_t raw = (uint32_t)id;
    if (raw == OPTICS_UI_ICON_NONE)
        return false;
    if (raw < OPTICS_UI_ICON_BUILTIN_COUNT)
        return true;
    return decode_registered(ui, id, nullptr);
}

const optics_ui_icon_desc *optics_ui__icon_desc(const optics_ui *ui, optics_ui_icon_id id) {
    uint32_t raw = (uint32_t)id;
    if (raw == OPTICS_UI_ICON_NONE)
        return nullptr;
    if (raw < OPTICS_UI_ICON_BUILTIN_COUNT)
        return &k_builtin_icons[raw];
    const optics_ui__icon_slot *slot = nullptr;
    if (!decode_registered(ui, id, &slot))
        return nullptr;
    return &slot->desc;
}

OPTICS_UI_API const optics_ui_icon_desc *optics_ui_icon_info(const optics_ui *ui,
                                                             optics_ui_icon_id id) {
    return optics_ui__icon_desc(ui, id);
}

static bool validate_desc(const optics_ui_icon_desc *desc) {
    if (!desc || !desc->cmds || desc->count == 0u || desc->count > OPTICS_UI_ICON_MAX_CMDS)
        return false;
    if (desc->paint > OPTICS_UI_ICON_PAINT_FILL)
        return false;
    for (uint32_t i = 0; i < desc->count; ++i) {
        if (desc->cmds[i].kind > OPTICS_UI_ICON_CMD_RECT)
            return false;
    }
    if (desc->run_count > 0u) {
        if (!desc->runs || desc->run_count > OPTICS_UI_ICON_MAX_RUNS)
            return false;
        for (uint32_t i = 0; i < desc->run_count; ++i) {
            const optics_ui_icon_run *r = &desc->runs[i];
            if (r->count == 0u || r->first_cmd > desc->count ||
                r->count > desc->count - r->first_cmd)
                return false;
            if (r->paint > OPTICS_UI_ICON_PAINT_FILL)
                return false;
        }
    }
    return true;
}

OPTICS_UI_API optics_ui_icon_id optics_ui_icon_register(optics_ui *ui,
                                                        const optics_ui_icon_desc *desc) {
    if (!ui || !validate_desc(desc))
        return OPTICS_UI_ICON_NONE;

    uint32_t slot_idx = UINT32_MAX;
    for (uint32_t i = 0; i < ui->icons.cap; ++i) {
        if (!ui->icons.slots[i].occupied) {
            slot_idx = i;
            break;
        }
    }

    if (slot_idx == UINT32_MAX) {
        if (ui->icons.cap >= OPTICS_UI_ICON_MAX_REGISTERED)
            return OPTICS_UI_ICON_NONE;
        uint32_t new_cap = ui->icons.cap ? ui->icons.cap * 2u : 16u;
        if (new_cap > OPTICS_UI_ICON_MAX_REGISTERED)
            new_cap = OPTICS_UI_ICON_MAX_REGISTERED;
        optics_ui__icon_slot *new_slots =
            optics_ui__alloc(ui, new_cap * sizeof(optics_ui__icon_slot));
        if (!new_slots)
            return OPTICS_UI_ICON_NONE;
        if (ui->icons.slots && ui->icons.cap > 0u) {
            memcpy(new_slots, ui->icons.slots, ui->icons.cap * sizeof(optics_ui__icon_slot));
            optics_ui__free(ui, ui->icons.slots);
        }
        slot_idx = ui->icons.cap;
        ui->icons.slots = new_slots;
        ui->icons.cap = new_cap;
    }

    optics_ui_icon_cmd *cmds_copy = optics_ui__alloc(ui, desc->count * sizeof(optics_ui_icon_cmd));
    if (!cmds_copy)
        return OPTICS_UI_ICON_NONE;
    memcpy(cmds_copy, desc->cmds, desc->count * sizeof(optics_ui_icon_cmd));

    optics_ui_icon_run *runs_copy = nullptr;
    if (desc->run_count > 0u) {
        runs_copy = optics_ui__alloc(ui, desc->run_count * sizeof(optics_ui_icon_run));
        if (!runs_copy) {
            optics_ui__free(ui, cmds_copy);
            return OPTICS_UI_ICON_NONE;
        }
        memcpy(runs_copy, desc->runs, desc->run_count * sizeof(optics_ui_icon_run));
    }

    optics_ui__icon_slot *slot = &ui->icons.slots[slot_idx];
    uint32_t gen = slot->generation + 1u;
    if (gen == 0u || gen > 0xFFFFu)
        gen = 1u;

    slot->generation = gen;
    slot->occupied = true;
    slot->cmds = cmds_copy;
    slot->runs = runs_copy;
    slot->desc = (optics_ui_icon_desc){
        .cmds = cmds_copy,
        .count = desc->count,
        .runs = runs_copy,
        .run_count = desc->run_count,
        .paint = desc->paint,
    };

    return (optics_ui_icon_id)((gen << OPTICS_UI_ICON_GEN_SHIFT) | slot_idx);
}

OPTICS_UI_API void optics_ui_icon_unregister(optics_ui *ui, optics_ui_icon_id id) {
    if (!ui || !ui->icons.slots)
        return;
    uint32_t raw = (uint32_t)id;
    uint32_t gen = raw >> OPTICS_UI_ICON_GEN_SHIFT;
    uint32_t idx = raw & OPTICS_UI_ICON_SLOT_MASK;
    if (gen == 0u || idx >= ui->icons.cap)
        return;
    optics_ui__icon_slot *slot = &ui->icons.slots[idx];
    if (!slot->occupied || slot->generation != gen)
        return;

    optics_ui__icon_mask_evict(ui, id);
    optics_ui__free(ui, slot->cmds);
    optics_ui__free(ui, slot->runs);
    slot->cmds = nullptr;
    slot->runs = nullptr;
    slot->desc = (optics_ui_icon_desc){};
    slot->occupied = false;
}
