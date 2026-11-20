/*
 * widget_gallery/page_typography.c — Typography scale, families, and scripts.
 */

#include "gallery.h"

void wg_page_typography(optics_ui *ui, const ds_palette *p, wg_app *app) {
    (void)app;
    ds_page_header(
        ui, p, "Typography",
        "HarfBuzz text shaping, dynamic R8 glyph atlas, font families, and BiDi layout.");

    ds_card_begin(ui, p, "Type Scale", "Consistent optical hierarchy across desktop densities.");
    optics_ui_label(ui, &(optics_ui_label_opts){
                            .text = "Display — 28px Semibold", .size = 28.0f, .weight = 650.0f});
    optics_ui_label(ui, &(optics_ui_label_opts){
                            .text = "Heading 1 — 22px Semibold", .size = 22.0f, .weight = 600.0f});
    optics_ui_label(ui, &(optics_ui_label_opts){
                            .text = "Heading 2 — 16px Medium", .size = 16.0f, .weight = 550.0f});
    optics_ui_label(ui, &(optics_ui_label_opts){
                            .text = "Body — 14px Regular. The quick brown fox jumps over the lazy "
                                    "dog. 0123456789.",
                            .size = 14.0f,
                        });
    optics_ui_label(ui,
                    &(optics_ui_label_opts){
                        .text = "Caption — 12px Secondary metadata and helper hints.",
                        .size = 12.0f,
                        .box = {.style = {.fields = OPTICS_UI_STYLE_FG, .fg = p->text_secondary}},
                    });
    ds_card_end(ui);

    ds_card_begin(ui, p, "Font Families",
                  "Runtime family switching between Sans-Serif, Serif, and Monospace.");
    optics_ui_text_family prev = optics_ui_get_text_family(ui);

    optics_ui_set_text_family(ui, OPTICS_UI_TEXT_FAMILY_SANS);
    optics_ui_label(ui, &(optics_ui_label_opts){
                            .text = "Sans-Serif: Clean geometric clarity for interface chrome.",
                            .size = 14.5f,
                        });

    optics_ui_set_text_family(ui, OPTICS_UI_TEXT_FAMILY_SERIF);
    optics_ui_label(ui, &(optics_ui_label_opts){
                            .text = "Serif: Editorial contrast suited for long-form reading.",
                            .size = 15.0f,
                        });

    optics_ui_set_text_family(ui, OPTICS_UI_TEXT_FAMILY_MONO);
    optics_ui_label(ui,
                    &(optics_ui_label_opts){
                        .text = "Monospace: optics_app_run(&(optics_app_opts){ .dark = true });",
                        .size = 13.5f,
                    });

    optics_ui_set_text_family(ui, prev);
    ds_card_end(ui);

    ds_card_begin(ui, p, "Multilingual & Complex Scripts",
                  "Automatic font fallback with HarfBuzz + FriBidi shaping.");
    optics_ui_label(
        ui, &(optics_ui_label_opts){
                .text = "Chinese / English: Optics 是基于 Vulkan 1.3 的现代 C23 图形与 UI 引擎。",
                .size = 14.5f,
            });
    optics_ui_label(ui, &(optics_ui_label_opts){
                            .text = "Japanese: 高速な即時モード UI と美しいタイポグラフィ。",
                            .size = 14.5f,
                        });
    optics_ui_label(ui, &(optics_ui_label_opts){
                            .text = "BiDi (Mixed LTR/RTL): Hello مرحبا بالعالم 2026!",
                            .size = 14.5f,
                        });
    ds_card_end(ui);
}
