/*
 * ADR-0085 borrow-checker self-test: the registry must catch the exact
 * misuse it exists for — a label pointer that was NOT registered this
 * frame — and must never fire on legitimate patterns.
 *
 * The library compiles with OPTICS_UI_DEBUG_BORROWS (always, in this repo),
 * so the registry is live. We exercise it three ways:
 *
 *   1. legitimate: heap-owned labels registered via normal widget
 *      builds across many frames — must not abort;
 *   2. arena-copied strings (placeholder) — exempt by construction;
 *   3. the violation itself: call optics_ui__gen_widget_id with a pointer
 *      that was never registered THIS frame (we deregister by resetting
 *      the registry, simulating frame N+1 receiving frame N's stack
 *      pointer). Must abort — verified by running this test as a
 *      subprocess that we EXPECT to die (see meson test is_fail).
 *
 * Legitimate-case assertions run in-process; the violation case is a
 * child process (abort) checked by exit status.
 */
#include "../../libs/ui/src/internal.h"
#include "optics_test.h"
#include "ui_test.h"
#include <optics/ui.h>

#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char **argv) {
    /* Violation mode: run from the parent as a fork child. */
    if (argc == 2 && strcmp(argv[1], "--violation") == 0) {
        optics_ui *ui = nullptr;
        if (optics_ui_create(&(optics_ui_desc){}, &ui) != OPTICS_GFX_OK)
            return 2;
        char frame_buf[32];
        optics_ui_input input = {};
        optics_ui_begin(ui, &input);

        /* Frame 1: register a stack pointer. */
        snprintf(frame_buf, sizeof frame_buf, "frame%u", (unsigned)ui->frame);
        optics_ui_label(ui, &(optics_ui_label_opts){.box = {.id = frame_buf}, .text = "x"});

        /* Simulate frame N+1 receiving frame N's stack pointer WITHOUT a
         * re-registration: clear the registry (as optics_ui_begin would) and
         * hand the stale pointer to a fresh check. The checker must
         * abort. */
        ui->borrow_count = 0;
        optics_ui__borrow_check(ui, frame_buf, "self-test violation");
        return 0; /* unreached if the checker works */
    }

    /* --- legitimate patterns, in-process -------------------------------- */
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    static char persistent_label[] = "persistent-label"; /* static storage: always fine */

    for (uint32_t frame = 0; frame < 64; frame++) {
        optics_ui_input input = {};
        optics_ui_begin(ui, &input);

        /* static storage: registered this frame, stable across frames */
        optics_ui_label(
            ui, &(optics_ui_label_opts){.box = {.id = persistent_label}, .text = persistent_label});

        /* heap-owned, fresh each frame: registered this frame */
        char *heap_label = malloc(32);
        snprintf(heap_label, 32, "heap-%u", (unsigned)frame);
        optics_ui_label(ui, &(optics_ui_label_opts){.box = {.id = heap_label}, .text = heap_label});
        free(heap_label);

        /* arena-copied placeholder: exempt by construction */
        char stack_placeholder[32];
        snprintf(stack_placeholder, sizeof stack_placeholder, "ph-%u", (unsigned)frame);
        static char ted_buf[16];
        ted_buf[0] = '\0';
        optics_ui_textedit(ui, &(optics_ui_textedit_opts){
                                   .box = {.id = "ted"},
                                   .buf = ted_buf,
                                   .cap = sizeof ted_buf,
                                   .placeholder = stack_placeholder,
                               });

        optics_ui_test_end(ui);
    }
    OT_EXPECT(!optics_ui_overflowed(ui));

    /* --- the violation, as a child process ------------------------------- */
    pid_t pid = fork();
    OT_EXPECT(pid >= 0);
    if (pid == 0) {
        execl("/proc/self/exe", argv[0], "--violation", (char *)nullptr);
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    /* The checker aborts => abnormal termination (SIGABRT). */
    OT_EXPECT(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);

    optics_ui_release(ui);
    return OT_REPORT();
}
