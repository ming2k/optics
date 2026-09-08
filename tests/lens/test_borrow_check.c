/*
 * ADR-0084 borrow-checker self-test: the registry must catch the exact
 * misuse it exists for — a label pointer that was NOT registered this
 * frame — and must never fire on legitimate patterns.
 *
 * The library compiles with LENS_DEBUG_BORROWS (always, in this repo),
 * so the registry is live. We exercise it three ways:
 *
 *   1. legitimate: heap-owned labels registered via normal widget
 *      builds across many frames — must not abort;
 *   2. arena-copied strings (placeholder) — exempt by construction;
 *   3. the violation itself: call lensi_gen_widget_id with a pointer
 *      that was never registered THIS frame (we deregister by resetting
 *      the registry, simulating frame N+1 receiving frame N's stack
 *      pointer). Must abort — verified by running this test as a
 *      subprocess that we EXPECT to die (see meson test is_fail).
 *
 * Legitimate-case assertions run in-process; the violation case is a
 * child process (abort) checked by exit status.
 */
#include "../../libs/lens/src/internal.h"
#include "test_helpers.h"
#include <lens/lens.h>

#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char **argv) {
    /* Violation mode: run from the parent as a fork child. */
    if (argc == 2 && strcmp(argv[1], "--violation") == 0) {
        lens *ui = NULL;
        if (lens_create(&(lens_desc){0}, &ui) != FLUX_OK)
            return 2;
        char frame_buf[32];
        lens_input input = {0};
        lens_begin(ui, &input);

        /* Frame 1: register a stack pointer. */
        snprintf(frame_buf, sizeof frame_buf, "frame%u", (unsigned)ui->frame);
        lens_label(ui, &(lens_label_opts){.box = {.id = frame_buf}, .text = "x"});

        /* Simulate frame N+1 receiving frame N's stack pointer WITHOUT a
         * re-registration: clear the registry (as lens_begin would) and
         * hand the stale pointer to a fresh check. The checker must
         * abort. */
        ui->borrow_count = 0;
        lensi_borrow_check(ui, frame_buf, "self-test violation");
        return 0; /* unreached if the checker works */
    }

    /* --- legitimate patterns, in-process -------------------------------- */
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    static char persistent_label[] = "persistent-label"; /* static storage: always fine */

    for (uint32_t frame = 0; frame < 64; frame++) {
        lens_input input = {0};
        lens_begin(ui, &input);

        /* static storage: registered this frame, stable across frames */
        lens_label(ui,
                   &(lens_label_opts){.box = {.id = persistent_label}, .text = persistent_label});

        /* heap-owned, fresh each frame: registered this frame */
        char *heap_label = malloc(32);
        snprintf(heap_label, 32, "heap-%u", (unsigned)frame);
        lens_label(ui, &(lens_label_opts){.box = {.id = heap_label}, .text = heap_label});
        free(heap_label);

        /* arena-copied placeholder: exempt by construction */
        char stack_placeholder[32];
        snprintf(stack_placeholder, sizeof stack_placeholder, "ph-%u", (unsigned)frame);
        static char ted_buf[16];
        ted_buf[0] = '\0';
        lens_textedit(ui, &(lens_textedit_opts){
                              .box = {.id = "ted"},
                              .buf = ted_buf,
                              .cap = sizeof ted_buf,
                              .placeholder = stack_placeholder,
                          });

        lens_end(ui);
    }
    CHECK(!lens_overflowed(ui));

    /* --- the violation, as a child process ------------------------------- */
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        execl("/proc/self/exe", argv[0], "--violation", (char *)NULL);
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    /* The checker aborts => abnormal termination (SIGABRT). */
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);

    lens_destroy(ui);
    return TEST_REPORT();
}
