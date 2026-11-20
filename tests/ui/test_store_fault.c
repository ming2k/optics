/* Compile the store against a faulting allocator: no GPU/platform hooks. */
#include "../../libs/ui/src/internal.h"
#include "lens_test.h"
#include "optics_test.h"

static int fail_after = -1;
static int allocations;
void *lensi_alloc(optics_ui *ui, size_t bytes) {
    (void)ui;
    if (fail_after == 0)
        return nullptr;
    if (fail_after > 0)
        fail_after--;
    void *p = malloc(bytes);
    if (p)
        allocations++;
    return p;
}
void lensi_free(optics_ui *ui, void *p) {
    (void)ui;
    if (p) {
        allocations--;
        free(p);
    }
}
void lensi_set_overflow(optics_ui *ui) {
    ui->overflow = true;
}
void lensi_node_release_cache(optics_ui *ui, optics_ui_node *n) {
    (void)ui;
    (void)n;
}
void lensi_node_reset_frame(optics_ui_node *n) {
    (void)n;
}

int main(void) {
    /* `lens` is large enough to overflow Windows' 1 MiB main-thread stack;
     * the test only needs its store, so keep it off the stack. */
    optics_ui *ui = calloc(1, sizeof *ui);
    OT_EXPECT(ui != nullptr);
    ui->frame = 1;
    OT_EXPECT(lensi_store_init(ui, UINT32_MAX) == OPTICS_GFX_ERROR_INVALID_ARGUMENT);
    OT_EXPECT(lensi_store_init(ui, 4) == OPTICS_GFX_OK);
    OT_EXPECT(lensi_store_touch(ui, 1) != nullptr);
    OT_EXPECT(lensi_store_touch(ui, 2) != nullptr);
    int before = allocations;
    fail_after = 1; /* node succeeds, hash-table growth fails */
    OT_EXPECT(lensi_store_touch(ui, 3) == nullptr);
    OT_EXPECT(ui->overflow);
    OT_EXPECT(lensi_store_find(ui, 3) == nullptr);
    OT_EXPECT(allocations == before);
    OT_EXPECT(ui->store.count == 2);
    fail_after = -1;
    OT_EXPECT(lensi_store_touch(ui, 3) != nullptr);
    optics_ui_node *nodes[1000];
    for (uint32_t i = 0; i < 1000; i++) {
        nodes[i] = lensi_store_touch(ui, i + 100);
        OT_EXPECT(nodes[i] != nullptr);
    }
    /* Interleaved victims/survivors exercise wraparound probe clusters and
     * relocation of the reap iterator. GC must work with all allocation off. */
    fail_after = 0;
    for (uint32_t f = 0; f < LENSI_LEAVE_GRACE_FRAMES + 2; f++) {
        ui->frame++;
        for (uint32_t i = 0; i < 1000; i += 2)
            OT_EXPECT(lensi_store_touch(ui, i + 100) == nodes[i]);
        lensi_store_reap(ui);
        for (uint32_t i = 0; i < 1000; i += 2)
            OT_EXPECT(lensi_store_find(ui, i + 100) == nodes[i]);
    }
    OT_EXPECT(ui->store.count == 500);
    for (uint32_t i = 1; i < 1000; i += 2)
        OT_EXPECT(lensi_store_find(ui, i + 100) == nullptr);
    lensi_store_destroy(ui);
    free(ui);
    OT_EXPECT(allocations == 0);
    return OT_REPORT();
}
