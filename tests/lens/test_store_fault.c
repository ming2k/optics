/* Compile the store against a faulting allocator: no GPU/platform hooks. */
#include "../../libs/lens/src/internal.h"
#include "test_helpers.h"

static int fail_after = -1;
static int allocations;
void *lensi_alloc(lens *ui, size_t bytes) {
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
void lensi_free(lens *ui, void *p) {
    (void)ui;
    if (p) {
        allocations--;
        free(p);
    }
}
void lensi_set_overflow(lens *ui) {
    ui->overflow = true;
}
void lensi_node_drop_record(lens *ui, lens_node *n) {
    (void)ui;
    (void)n;
}
void lensi_node_reset_frame(lens_node *n) {
    (void)n;
}

int main(void) {
    lens ui = {.frame = 1};
    CHECK(lensi_store_init(&ui, UINT32_MAX) == FLUX_ERROR_INVALID_ARGUMENT);
    CHECK(lensi_store_init(&ui, 4) == FLUX_OK);
    CHECK(lensi_store_touch(&ui, 1) != nullptr);
    CHECK(lensi_store_touch(&ui, 2) != nullptr);
    int before = allocations;
    fail_after = 1; /* node succeeds, hash-table growth fails */
    CHECK(lensi_store_touch(&ui, 3) == nullptr);
    CHECK(ui.overflow);
    CHECK(lensi_store_find(&ui, 3) == nullptr);
    CHECK(allocations == before);
    CHECK(ui.store.count == 2);
    fail_after = -1;
    CHECK(lensi_store_touch(&ui, 3) != nullptr);
    lens_node *nodes[1000];
    for (uint32_t i = 0; i < 1000; i++) {
        nodes[i] = lensi_store_touch(&ui, i + 100);
        CHECK(nodes[i] != nullptr);
    }
    /* Interleaved victims/survivors exercise wraparound probe clusters and
     * relocation of the reap iterator. GC must work with all allocation off. */
    fail_after = 0;
    for (uint32_t f = 0; f < LENSI_LEAVE_GRACE_FRAMES + 2; f++) {
        ui.frame++;
        for (uint32_t i = 0; i < 1000; i += 2)
            CHECK(lensi_store_touch(&ui, i + 100) == nodes[i]);
        lensi_store_reap(&ui);
        for (uint32_t i = 0; i < 1000; i += 2)
            CHECK(lensi_store_find(&ui, i + 100) == nodes[i]);
    }
    CHECK(ui.store.count == 500);
    for (uint32_t i = 1; i < 1000; i += 2)
        CHECK(lensi_store_find(&ui, i + 100) == nullptr);
    lensi_store_destroy(&ui);
    CHECK(allocations == 0);
    return TEST_REPORT();
}
