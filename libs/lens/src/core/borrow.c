/* borrow.c — debug-only borrowed-string registry (ADR-0084).
 *
 * The lens API hands caller-owned strings to widgets (`label`, `box.id`,
 * `placeholder`) and documents that they must remain stable through the
 * frame. The failure mode is passing a stack temporary: frame N works,
 * frame N+1 reads freed stack. Nothing else in the library can catch
 * this — the pointer is only stored, never dereferenced at registration
 * time.
 *
 * Mechanism (debug builds only, LENS_DEBUG_BORROWS):
 *   - every widget's label/id/placeholder pointer is REGISTERED in a
 *     bounded frame-local set when the widget consumes it;
 *   - anything the library itself copies into the frame arena is exempt
 *     by construction (any pointer inside the arena is arena-owned);
 *   - the check runs against the CURRENT frame's registry. A pointer
 *     registered last frame but not this one is exactly the dangling
 *     case (stale stack pointer, freed heap string, or a caller who
 *     mutated the string between frames).
 *
 * Zero release-build cost: the registry compiles out entirely. */

#include "../internal.h"

#include <stdio.h>
#include <stdlib.h>

void lensi_borrow_register(lens *ui, const char *ptr) {
    if (!ui || !ptr)
        return;
    /* Bounded ring: at capacity, drop the oldest. The check below only
     * needs recent registrations (a widget registers and reads within a
     * few hundred builds); wrapping away ancient entries weakens the
     * net, never misfires it. */
    if (ui->borrow_count == LENSI_BORROW_SET_MAX) {
        memmove(ui->borrow_set, ui->borrow_set + 1,
                (LENSI_BORROW_SET_MAX - 1) * sizeof ui->borrow_set[0]);
        ui->borrow_count--;
    }
    ui->borrow_set[ui->borrow_count++] = ptr;
}

void lensi_borrow_check(lens *ui, const char *ptr, const char *what) {
    if (!ui || !ptr)
        return;
    /* Arena-owned pointers are always fine: the arena lives for the
     * frame and the library copied the bytes there itself. This also
     * covers the semantics/line copies (arena_strn / push_line_slice)
     * and the textedit preedit display buffer. */
    if (ptr >= (const char *)ui->arena.base &&
        ptr < (const char *)ui->arena.base + ui->arena.capacity)
        return;

    for (uint32_t i = 0; i < ui->borrow_count; ++i) {
        if (ui->borrow_set[i] == ptr)
            return;
    }
    fprintf(stderr,
            "lens borrow-check FAILED: %s pointer %p was not registered this frame\n"
            "  most likely: a stack temporary or freed buffer passed to a lens widget\n"
            "  (label/id/placeholder strings must stay stable through the frame;\n"
            "  copy into caller-owned storage if they cannot). See lens.h ADR-0084.\n",
            what, (const void *)ptr);
    abort();
}
