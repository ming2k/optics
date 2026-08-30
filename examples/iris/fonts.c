/* fonts.c — text rendering showcase. */

#include <iris/app.h>
#include <iris/window.h>
#include <lens/lens.h>

#include <stdio.h>

static void line(lens *ui, float size, const char *text) {
    lens_size(ui, 0, size * 1.6f);
    lens_label(ui, &(lens_label_opts){.text = text, .size = size});
}

static void build(lens *ui, const lens_input *in, void *user) {
    (void)user;

    /* Esc quits (the startup line says so). */
    for (uint32_t k = 0; k < in->key_count; k++)
        if (in->keys[k].pressed && in->keys[k].key == LENS_KEY_ESCAPE)
            iris_window_close();
    lens_column_begin(ui, &(lens_layout_opts){.pad = 28,
                                              .gap = 6,
                                              .cross = LENS_START,
                                              .bg = flux_color_rgba(0x1e, 0x1e, 0x24, 0xff)});

    line(ui, 40.0f, "iris text");
    line(ui, 22.0f, "The quick brown fox jumps over the lazy dog.");
    line(ui, 18.0f, "Shaped with HarfBuzz, rasterised with FreeType.");
    line(ui, 16.0f, "Counters render correctly: a e o g B R 8 @ & %");
    line(ui, 14.0f, "abcdefghijklmnopqrstuvwxyz  0123456789");
    line(ui, 14.0f, "ABCDEFGHIJKLMNOPQRSTUVWXYZ  ()[]{}<>/\\");
    line(ui, 12.0f, "Tiny 12px body text stays legible at HiDPI.");

    lens_close(ui);
}

int main(void) {
    printf("iris fonts demo. Font via fontconfig (XDG search);\n"
           "override with $FLUX_TEXT_FONT. Esc quits.\n\n");
    return iris_app_run(&(iris_app_config){
        .title = "iris — fonts", .width = 760, .height = 460, .dark = true, .build = build});
}
