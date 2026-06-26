/* minimal.c — the smallest useful iris program.
 *
 * One label and one button on a native Wayland window. This is the
 * "hello world": the whole UI is the build callback below; iris Wayland backend
 * owns the window, Vulkan surface, input, and frame loop.
 */

#include <iris/app.h>
#include <lens/lens.h>

#include <stdio.h>

static void build(lens *ui, const lens_input *in, void *user) {
    (void)user;
    lens_column_ex(ui, (lens_layout_opts){.pad = 24, .gap = 14, .cross = LENS_START});
    lens_label(ui, "Hello from iris");
    if (lens_button(ui, "Click me"))
        printf("clicked!\n");
    lens_close(ui);
}

int main(void) {
    printf("iris minimal — click the button (prints here). Esc quits.\n\n");
    return iris_app_run(&(iris_app_config){
        .title = "iris — minimal",
        .width = 360,
        .height = 180,
        .dark = true,
        .build = build,
    });
}
