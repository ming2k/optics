/* test_place_occlusion.c — a placed node in a higher band must swallow
 * interactions aimed at widgets beneath it, while its own children stay
 * interactive (ADR-0060: occlusion IS the hit-test order). */

#include "lens_test.h"
#include "optics_test.h"
#include <optics/gfx/canvas_cpu.h>
#include <optics/ui.h>
#include <string.h>

static const optics_ui_input IN0 = {.display_size = {400, 300}, .dt_seconds = 0.016f};

static optics_ui_place_opts card_opts(optics_gfx_rect rect) {
    return (optics_ui_place_opts){
        .box = {.id = "hover-card"},
        .band = OPTICS_UI_BAND_POPUP,
        .mode = OPTICS_UI_PLACE_EXACT,
        .rect = rect,
        .layout =
            {
                .box = {.min_width = 260.0f},
                .pad = 10.0f,
                .gap = 4.0f,
                .bg = 0xFA20202Cu,
                .border = 0x12FFFFFFu,
                .border_width = 1.0f,
                .radius = 12.0f,
                .cross = OPTICS_UI_STRETCH,
            },
    };
}

static void test_popup_card_occludes_base_widget(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    bool base_clicked = false;
    bool card_clicked = false;

    for (int frame = 0; frame < 4; ++frame) {
        optics_ui_input in = IN0;
        if (frame == 2)
            in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = true;
        if (frame == 3)
            in.mouse_released[OPTICS_UI_MOUSE_LEFT] = true;
        if (frame >= 2)
            in.cursor = (optics_gfx_point){40.0f, 40.0f};

        optics_ui_begin(ui, &in);

        /* base content: a button the card will cover */
        if (optics_ui_button(ui, &(optics_ui_button_opts){.label = "covered playlist row"}).clicked)
            base_clicked = true;

        /* floating card over the same spot */
        optics_ui_place_opts opts = card_opts((optics_gfx_rect){20.0f, 20.0f, 260.0f, 0.0f});
        optics_ui_place_begin(ui, &opts);
        if (optics_ui_button(ui, &(optics_ui_button_opts){.label = "card button"}).clicked)
            card_clicked = true;
        optics_ui_place_end(ui);

        optics_ui_test_end(ui);
    }

    OT_EXPECT(!base_clicked);
    OT_EXPECT(card_clicked);
    optics_ui_release(ui);
}

static optics_ui_place_opts popup_at(const char *id, optics_gfx_rect rect) {
    return (optics_ui_place_opts){
        .box = {.id = id},
        .band = OPTICS_UI_BAND_POPUP,
        .mode = OPTICS_UI_PLACE_EXACT,
        .rect = rect,
        .layout = {.pad = 8.0f, .bg = 0xFF202020u, .cross = OPTICS_UI_STRETCH},
    };
}

static void test_later_popup_occludes_earlier_same_band(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    const optics_gfx_rect R1 = {0, 0, 200, 200};
    const optics_gfx_rect R2 = {100, 0, 200, 200};

#define BUILD_TWO()                                                                                \
    do {                                                                                           \
        optics_ui_place_opts o1 = popup_at("first", R1);                                           \
        if (optics_ui_place_begin(ui, &o1)) {                                                      \
            (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "FirstBtn"});             \
            first_r = optics_ui_get_response(ui);                                                  \
            optics_ui_place_end(ui);                                                               \
        }                                                                                          \
        optics_ui_place_opts o2 = popup_at("second", R2);                                          \
        if (optics_ui_place_begin(ui, &o2)) {                                                      \
            (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "SecondBtn"});            \
            second_r = optics_ui_get_response(ui);                                                 \
            optics_ui_place_end(ui);                                                               \
        }                                                                                          \
    } while (0)

    optics_ui_response first_r = {}, second_r = {};
    optics_ui_begin(ui, &IN0);
    BUILD_TWO();
    optics_ui_test_end(ui);

    optics_ui_input in = IN0;
    in.cursor = (optics_gfx_point){150, 20};
    optics_ui_begin(ui, &in);
    BUILD_TWO();
    optics_ui_test_end(ui);
    OT_EXPECT(!first_r.hovered);
    OT_EXPECT(second_r.hovered);

    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = true;
    in.mouse_down[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    BUILD_TWO();
    optics_ui_test_end(ui);
    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = false;
    in.mouse_down[OPTICS_UI_MOUSE_LEFT] = false;
    in.mouse_released[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    BUILD_TWO();
    optics_ui_test_end(ui);
    OT_EXPECT(!first_r.clicked);
    OT_EXPECT(second_r.clicked);

#undef BUILD_TWO
    optics_ui_release(ui);
}

int main(void) {
    test_popup_card_occludes_base_widget();
    test_later_popup_occludes_earlier_same_band();
    return OT_REPORT();
}
