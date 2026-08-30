/* test_place_occlusion.c — a placed node in a higher band must swallow
 * interactions aimed at widgets beneath it, while its own children stay
 * interactive (ADR-0060: occlusion IS the hit-test order). */

#include "test_helpers.h"
#include <flux/canvas_cpu.h>
#include <lens/lens.h>
#include <string.h>

static const lens_input IN0 = {.display_size = {400, 300}, .dt_seconds = 0.016f};

static lens_place_opts card_opts(flux_rect rect) {
    return (lens_place_opts){
        .box = {.id = "hover-card"},
        .band = LENS_BAND_POPUP,
        .mode = LENS_PLACE_EXACT,
        .rect = rect,
        .layout =
            {
                .pad = 10.0f,
                .gap = 4.0f,
                .bg = 0xFA20202Cu,
                .border = 0x12FFFFFFu,
                .border_width = 1.0f,
                .radius = 12.0f,
                .min_width = 260.0f,
                .cross = LENS_STRETCH,
            },
    };
}

static void test_popup_card_occludes_base_widget(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    bool base_clicked = false;
    bool card_clicked = false;

    for (int frame = 0; frame < 4; ++frame) {
        lens_input in = IN0;
        if (frame == 2)
            in.mouse_pressed[LENS_MOUSE_LEFT] = true;
        if (frame == 3)
            in.mouse_released[LENS_MOUSE_LEFT] = true;
        if (frame >= 2)
            in.cursor = (flux_point){40.0f, 40.0f};

        lens_begin(ui, &in);

        /* base content: a button the card will cover */
        if (lens_button(ui, &(lens_button_opts){.label = "covered playlist row"}).clicked)
            base_clicked = true;

        /* floating card over the same spot */
        lens_place_opts opts = card_opts((flux_rect){20.0f, 20.0f, 260.0f, 0.0f});
        lens_place_begin(ui, &opts);
        if (lens_button(ui, &(lens_button_opts){.label = "card button"}).clicked)
            card_clicked = true;
        lens_place_end(ui);

        lens_end(ui);
    }

    CHECK(!base_clicked);
    CHECK(card_clicked);
    lens_destroy(ui);
}

static lens_place_opts popup_at(const char *id, flux_rect rect) {
    return (lens_place_opts){
        .box = {.id = id},
        .band = LENS_BAND_POPUP,
        .mode = LENS_PLACE_EXACT,
        .rect = rect,
        .layout = {.pad = 8.0f, .bg = 0xFF202020u, .cross = LENS_STRETCH},
    };
}

static void test_later_popup_occludes_earlier_same_band(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    const flux_rect R1 = {0, 0, 200, 200};
    const flux_rect R2 = {100, 0, 200, 200};

#define BUILD_TWO()                                                                                \
    do {                                                                                           \
        lens_place_opts o1 = popup_at("first", R1);                                                \
        if (lens_place_begin(ui, &o1)) {                                                           \
            (void)lens_button(ui, &(lens_button_opts){.label = "FirstBtn"});                       \
            first_r = lens_get_response(ui);                                                       \
            lens_place_end(ui);                                                                    \
        }                                                                                          \
        lens_place_opts o2 = popup_at("second", R2);                                               \
        if (lens_place_begin(ui, &o2)) {                                                           \
            (void)lens_button(ui, &(lens_button_opts){.label = "SecondBtn"});                      \
            second_r = lens_get_response(ui);                                                      \
            lens_place_end(ui);                                                                    \
        }                                                                                          \
    } while (0)

    lens_response first_r = {0}, second_r = {0};
    lens_begin(ui, &IN0);
    BUILD_TWO();
    lens_end(ui);

    lens_input in = IN0;
    in.cursor = (flux_point){150, 20};
    lens_begin(ui, &in);
    BUILD_TWO();
    lens_end(ui);
    CHECK(!first_r.hovered);
    CHECK(second_r.hovered);

    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    in.mouse_down[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    BUILD_TWO();
    lens_end(ui);
    in.mouse_pressed[LENS_MOUSE_LEFT] = false;
    in.mouse_down[LENS_MOUSE_LEFT] = false;
    in.mouse_released[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    BUILD_TWO();
    lens_end(ui);
    CHECK(!first_r.clicked);
    CHECK(second_r.clicked);

#undef BUILD_TWO
    lens_destroy(ui);
}

int main(void) {
    test_popup_card_occludes_base_widget();
    test_later_popup_occludes_earlier_same_band();
    return TEST_REPORT();
}
