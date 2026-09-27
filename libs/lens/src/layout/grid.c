/* grid.c — two-dimensional grid track placement, track sizing, and cell arrangement (ADR-0104). */

#include "../internal.h"

/* ================================================================== */
/*  Occupancy bitset                                                  */
/* ================================================================== */

typedef struct grid_occ {
    uint64_t *bits;
    uint32_t words_per_row;
    uint32_t capacity_rows;
    lens *ui;
} grid_occ;

static void grid_occ_init(grid_occ *occ, lens *ui, uint32_t cols, uint32_t initial_rows,
                          uint64_t *stack_buf, uint32_t stack_words) {
    occ->ui = ui;
    occ->words_per_row = (cols + 63) / 64;
    if (occ->words_per_row == 0)
        occ->words_per_row = 1;
    occ->capacity_rows = initial_rows > 0 ? initial_rows : 16;
    uint32_t total_words = occ->capacity_rows * occ->words_per_row;
    if (stack_buf && total_words <= stack_words) {
        occ->bits = stack_buf;
    } else {
        occ->bits = (uint64_t *)flux_arena_alloc(&ui->arena, total_words * sizeof(uint64_t));
    }
    if (occ->bits)
        memset(occ->bits, 0, total_words * sizeof(uint64_t));
}

static void grid_occ_ensure_cap(grid_occ *occ, uint32_t needed_rows) {
    if (needed_rows <= occ->capacity_rows)
        return;
    uint32_t new_cap = occ->capacity_rows * 2;
    if (needed_rows > new_cap)
        new_cap = needed_rows + 16;
    uint32_t old_words = occ->capacity_rows * occ->words_per_row;
    uint32_t new_words = new_cap * occ->words_per_row;
    uint64_t *new_bits =
        (uint64_t *)flux_arena_alloc(&occ->ui->arena, new_words * sizeof(uint64_t));
    if (new_bits) {
        memset(new_bits, 0, new_words * sizeof(uint64_t));
        if (occ->bits)
            memcpy(new_bits, occ->bits, old_words * sizeof(uint64_t));
        occ->bits = new_bits;
        occ->capacity_rows = new_cap;
    }
}

static bool grid_occ_is_set(const grid_occ *occ, uint32_t r, uint32_t c) {
    if (r >= occ->capacity_rows || !occ->bits || (c / 64) >= occ->words_per_row)
        return false;
    uint32_t idx = r * occ->words_per_row + (c / 64);
    return (occ->bits[idx] & (1ULL << (c % 64))) != 0;
}

static void grid_occ_set(grid_occ *occ, uint32_t r, uint32_t c) {
    grid_occ_ensure_cap(occ, r + 1);
    if (!occ->bits || (c / 64) >= occ->words_per_row)
        return;
    uint32_t idx = r * occ->words_per_row + (c / 64);
    occ->bits[idx] |= (1ULL << (c % 64));
}

static bool grid_block_is_free(const grid_occ *occ, uint32_t r, uint32_t c, uint32_t span_r,
                               uint32_t span_c) {
    for (uint32_t ri = r; ri < r + span_r; ri++) {
        if (ri >= occ->capacity_rows)
            continue;
        for (uint32_t ci = c; ci < c + span_c; ci++) {
            if (grid_occ_is_set(occ, ri, ci))
                return false;
        }
    }
    return true;
}

static void grid_block_occupy(grid_occ *occ, uint32_t r, uint32_t c, uint32_t span_r,
                              uint32_t span_c) {
    grid_occ_ensure_cap(occ, r + span_r);
    for (uint32_t ri = r; ri < r + span_r; ri++) {
        for (uint32_t ci = c; ci < c + span_c; ci++) {
            grid_occ_set(occ, ri, ci);
        }
    }
}

/* ================================================================== */
/*  2D Placement phase                                                */
/* ================================================================== */

static uint32_t grid_place_children(lens_node *n) {
    uint32_t cols = n->grid_columns > 0 ? n->grid_columns : 1;
    uint64_t stack_buf[64];
    grid_occ occ;
    grid_occ_init(&occ, n->ui, cols, 16, stack_buf, 64);

    /* Phase 1: Explicitly placed children */
    for (lens_node *c = n->first_child; c; c = c->next_sibling) {
        if (c->place == LENS_PLACE_ABS)
            continue;
        if (c->grid_col > 0 && c->grid_row > 0) {
            uint32_t col = (uint32_t)(c->grid_col - 1);
            uint32_t row = (uint32_t)(c->grid_row - 1);
            if (col >= cols)
                col = cols - 1;
            uint32_t span_c = c->col_span > 0 ? c->col_span : 1;
            if (col + span_c > cols)
                span_c = cols - col;
            uint32_t span_r = c->row_span > 0 ? c->row_span : 1;

            grid_block_occupy(&occ, row, col, span_r, span_c);
            c->resolved_col = col;
            c->resolved_row = row;
            c->resolved_col_span = span_c;
            c->resolved_row_span = span_r;
        }
    }

    /* Phase 2: Flow / auto-placed children */
    for (lens_node *c = n->first_child; c; c = c->next_sibling) {
        if (c->place == LENS_PLACE_ABS)
            continue;
        if (c->grid_col > 0 && c->grid_row > 0)
            continue; /* already placed */

        uint32_t span_c = c->col_span > 0 ? c->col_span : 1;
        if (span_c > cols)
            span_c = cols;
        uint32_t span_r = c->row_span > 0 ? c->row_span : 1;

        uint32_t target_r = 0, target_c = 0;
        if (c->grid_col > 0) {
            target_c = (uint32_t)(c->grid_col - 1);
            if (target_c >= cols)
                target_c = cols - 1;
            if (target_c + span_c > cols)
                span_c = cols - target_c;
            uint32_t r = 0;
            for (;; r++) {
                if (grid_block_is_free(&occ, r, target_c, span_r, span_c)) {
                    target_r = r;
                    break;
                }
            }
        } else if (c->grid_row > 0) {
            target_r = (uint32_t)(c->grid_row - 1);
            bool found = false;
            for (uint32_t r = target_r;; r++) {
                for (uint32_t ci = 0; ci + span_c <= cols; ci++) {
                    if (grid_block_is_free(&occ, r, ci, span_r, span_c)) {
                        target_r = r;
                        target_c = ci;
                        found = true;
                        break;
                    }
                }
                if (found)
                    break;
            }
        } else {
            /* Pure auto-placement (dense packing for compact Bento/dashboard layouts) */
            for (uint32_t r = 0;; r++) {
                bool found = false;
                for (uint32_t ci = 0; ci + span_c <= cols; ci++) {
                    if (grid_block_is_free(&occ, r, ci, span_r, span_c)) {
                        target_r = r;
                        target_c = ci;
                        found = true;
                        break;
                    }
                }
                if (found)
                    break;
            }
        }

        grid_block_occupy(&occ, target_r, target_c, span_r, span_c);
        c->resolved_col = target_c;
        c->resolved_row = target_r;
        c->resolved_col_span = span_c;
        c->resolved_row_span = span_r;
    }

    uint32_t total_rows = 0;
    for (lens_node *c = n->first_child; c; c = c->next_sibling) {
        if (c->place == LENS_PLACE_ABS)
            continue;
        uint32_t end_r = c->resolved_row + c->resolved_row_span;
        if (end_r > total_rows)
            total_rows = end_r;
    }
    return total_rows;
}

static void grid_compute_row_heights(const lens_node *n, uint32_t total_rows, float *row_h) {
    if (total_rows == 0)
        return;
    memset(row_h, 0, total_rows * sizeof(float));
    if (n->grid_row_height > 0.0f) {
        for (uint32_t r = 0; r < total_rows; r++)
            row_h[r] = n->grid_row_height;
        return;
    }

    /* 1-span items first */
    for (const lens_node *c = n->first_child; c; c = c->next_sibling) {
        if (c->place == LENS_PLACE_ABS || c->resolved_row_span != 1)
            continue;
        uint32_t r = c->resolved_row;
        if (r < total_rows && c->measured.y > row_h[r])
            row_h[r] = c->measured.y;
    }

    /* Multi-span items: distribute deficit */
    for (const lens_node *c = n->first_child; c; c = c->next_sibling) {
        if (c->place == LENS_PLACE_ABS || c->resolved_row_span <= 1)
            continue;
        uint32_t r = c->resolved_row;
        uint32_t span = c->resolved_row_span;
        uint32_t count = 0;
        float cur_h = (span > 1) ? (float)(span - 1) * n->grid_row_gap : 0.0f;
        for (uint32_t i = 0; i < span && r + i < total_rows; i++) {
            cur_h += row_h[r + i];
            count++;
        }
        if (count > 0 && c->measured.y > cur_h) {
            float deficit = (c->measured.y - cur_h) / (float)count;
            for (uint32_t i = 0; i < count; i++)
                row_h[r + i] += deficit;
        }
    }
}

/* ================================================================== */
/*  Public solver entries                                             */
/* ================================================================== */

flux_point lensi_grid_measure(lens_node *n) {
    uint32_t cols = n->grid_columns > 0 ? n->grid_columns : 1;
    uint32_t total_rows = grid_place_children(n);
    if (total_rows == 0) {
        return (flux_point){2.0f * n->pad, 2.0f * n->pad};
    }

    float *row_h = (float *)flux_arena_alloc(&n->ui->arena, total_rows * sizeof(float));
    grid_compute_row_heights(n, total_rows, row_h);

    float *col_w = (float *)flux_arena_alloc(&n->ui->arena, cols * sizeof(float));
    memset(col_w, 0, cols * sizeof(float));

    /* 1-span items first */
    for (const lens_node *c = n->first_child; c; c = c->next_sibling) {
        if (c->place == LENS_PLACE_ABS || c->resolved_col_span != 1)
            continue;
        uint32_t ci = c->resolved_col;
        if (ci < cols && c->measured.x > col_w[ci])
            col_w[ci] = c->measured.x;
    }
    /* Multi-span items */
    for (const lens_node *c = n->first_child; c; c = c->next_sibling) {
        if (c->place == LENS_PLACE_ABS || c->resolved_col_span <= 1)
            continue;
        uint32_t ci = c->resolved_col;
        uint32_t span = c->resolved_col_span;
        uint32_t count = 0;
        float cur_w = (span > 1) ? (float)(span - 1) * n->gap : 0.0f;
        for (uint32_t i = 0; i < span && ci + i < cols; i++) {
            cur_w += col_w[ci + i];
            count++;
        }
        if (count > 0 && c->measured.x > cur_w) {
            float deficit = (c->measured.x - cur_w) / (float)count;
            for (uint32_t i = 0; i < count; i++)
                col_w[ci + i] += deficit;
        }
    }

    float max_col = 0.0f;
    for (uint32_t ci = 0; ci < cols; ci++) {
        if (col_w[ci] > max_col)
            max_col = col_w[ci];
    }
    float sum_w =
        (float)cols * max_col + (cols > 1 ? (float)(cols - 1) * n->gap : 0.0f) + 2.0f * n->pad;

    float sum_h = 0.0f;
    for (uint32_t r = 0; r < total_rows; r++)
        sum_h += row_h[r];
    if (total_rows > 1)
        sum_h += (float)(total_rows - 1) * n->grid_row_gap;
    sum_h += 2.0f * n->pad;

    return (flux_point){sum_w, sum_h};
}

void lensi_grid_arrange(lens_node *n, flux_rect inner) {
    uint32_t cols = n->grid_columns > 0 ? n->grid_columns : 1;
    uint32_t total_rows = 0;
    for (lens_node *c = n->first_child; c; c = c->next_sibling) {
        if (c->place == LENS_PLACE_ABS)
            continue;
        uint32_t end_r = c->resolved_row + c->resolved_row_span;
        if (end_r > total_rows)
            total_rows = end_r;
    }
    if (cols == 0 || total_rows == 0) {
        for (lens_node *c = n->first_child; c; c = c->next_sibling)
            if (c->place == LENS_PLACE_ABS)
                lensi_arrange_node(c, lensi_resolve_abs_rect(n->ui, c));
        return;
    }

    float avail_w = fmaxf(0.0f, inner.w - (cols > 1 ? (float)(cols - 1) * n->gap : 0.0f));
    float unit_col_w = cols > 0 ? (avail_w / (float)cols) : 0.0f;

    float *row_h = (float *)flux_arena_alloc(&n->ui->arena, total_rows * sizeof(float));
    grid_compute_row_heights(n, total_rows, row_h);

    float *row_y = (float *)flux_arena_alloc(&n->ui->arena, (total_rows + 1) * sizeof(float));
    float cur_y = inner.y;
    for (uint32_t r = 0; r < total_rows; r++) {
        row_y[r] = cur_y;
        cur_y += row_h[r] + n->grid_row_gap;
    }

    for (lens_node *c = n->first_child; c; c = c->next_sibling) {
        if (c->place == LENS_PLACE_ABS)
            continue;
        uint32_t col = c->resolved_col;
        uint32_t row = c->resolved_row;
        uint32_t c_span = c->resolved_col_span > 0 ? c->resolved_col_span : 1;
        uint32_t r_span = c->resolved_row_span > 0 ? c->resolved_row_span : 1;
        if (col >= cols)
            col = cols - 1;
        if (col + c_span > cols)
            c_span = cols - col;

        float cell_x = inner.x + (float)col * (unit_col_w + n->gap);
        float cell_y = row < total_rows ? row_y[row] : inner.y;
        float cell_w =
            (float)c_span * unit_col_w + (c_span > 1 ? (float)(c_span - 1) * n->gap : 0.0f);
        float cell_h = 0.0f;
        for (uint32_t i = 0; i < r_span && row + i < total_rows; i++)
            cell_h += row_h[row + i];
        if (r_span > 1)
            cell_h += (float)(r_span - 1) * n->grid_row_gap;

        float target_w = (c->fixed_w > 0.0f || c->fit) ? c->measured.x : cell_w;
        target_w = lensi_constrain_extent(target_w, c->min_w, c->max_w);
        if (target_w > cell_w && c->min_w <= cell_w)
            target_w = cell_w;

        float target_h = (c->fixed_h > 0.0f || c->fit) ? c->measured.y : cell_h;
        target_h = lensi_constrain_extent(target_h, c->min_h, c->max_h);
        if (target_h > cell_h && c->min_h <= cell_h)
            target_h = cell_h;

        float off_x = lensi_align_offset(n->align, cell_w - target_w);
        float off_y = lensi_align_offset(n->cross, cell_h - target_h);

        lensi_arrange_node(c, (flux_rect){cell_x + off_x, cell_y + off_y, target_w, target_h});
    }

    for (lens_node *c = n->first_child; c; c = c->next_sibling)
        if (c->place == LENS_PLACE_ABS)
            lensi_arrange_node(c, lensi_resolve_abs_rect(n->ui, c));
}
