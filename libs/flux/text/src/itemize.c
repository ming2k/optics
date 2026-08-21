/* itemize.c — split a UTF-8 string into runs uniform in direction, script and
 * font face, emitted in visual (left-to-right) order.
 *
 * Pipeline: decode to codepoints -> FriBidi embedding levels -> split on
 * (level, script, face) boundaries in logical order -> reorder the run list to
 * visual order (UBA rule L2). HarfBuzz then shapes each run with the right
 * direction/script, and the layout pass advances a single L->R pen across the
 * visually-ordered runs.
 *
 * Working-set policy: every scratch array (codepoints, byte offsets,
 * bidi types, embedding levels, and the run list + per-run levels)
 * lives on the flux_text context and follows one high-water rule —
 * grow to the largest input ever seen, reuse thereafter, release on
 * shutdown or flux_text_compact. An earlier version malloc/free'd the
 * four codepoint arrays per call past a 256-entry stack fallback,
 * which meant an 8-branch manual cleanup at every early exit (the
 * classic leak-on-error shape) and a transient ~13 B/byte peak that
 * the caller could not observe or reclaim. bos uses uint32_t (not
 * size_t) since strings > 4 GiB are rejected at the door, halving the
 * per-byte working set. */

#include "text_internal.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Resolve a per-codepoint script, letting Common/Inherited extend the run. */
static hb_script_t resolve_script(hb_unicode_funcs_t *uf, uint32_t cp, hb_script_t cur) {
    hb_script_t sc = hb_unicode_script(uf, cp);
    if (sc == HB_SCRIPT_COMMON || sc == HB_SCRIPT_INHERITED || sc == HB_SCRIPT_UNKNOWN)
        return cur != HB_SCRIPT_INVALID ? cur : sc;
    return sc;
}

/* UBA L2: reverse contiguous run sequences from the highest level down to the
 * lowest odd level, turning a logical-order run list into visual order. */
static void reorder_runs_visual(text_run *runs, const FriBidiLevel *levels, int n) {
    if (n <= 1)
        return;

    FriBidiLevel max_level = 0;
    FriBidiLevel min_odd = 127;
    for (int i = 0; i < n; i++) {
        if (levels[i] > max_level)
            max_level = levels[i];
        if ((levels[i] & 1) && levels[i] < min_odd)
            min_odd = levels[i];
    }
    if (min_odd > max_level)
        return; /* no RTL runs */

    for (FriBidiLevel lvl = max_level; lvl >= min_odd; lvl--) {
        int i = 0;
        while (i < n) {
            if (levels[i] < lvl) {
                i++;
                continue;
            }
            int j = i;
            while (j < n && levels[j] >= lvl)
                j++;
            /* reverse runs [i, j) */
            for (int a = i, b = j - 1; a < b; a++, b--) {
                text_run tmp = runs[a];
                runs[a] = runs[b];
                runs[b] = tmp;
            }
            i = j;
        }
    }
}

/* Grow t->runs_buf / t->run_levels_buf to at least `need` entries. Returns
 * false on allocation failure (buffers left untouched). The growth is
 * permanent for the context lifetime (or until flux_text_compact) — same
 * high-water policy as layout_buf. */
static bool runs_reserve(flux_text *t, int need) {
    if (need <= t->runs_cap)
        return true;
    int cap = t->runs_cap ? t->runs_cap : TXT_RUNS_INIT;
    while (cap < need) {
        if (cap > INT_MAX / 2)
            return false;
        cap *= 2;
    }

    /* Grow transactionally: two independent realloc calls cannot preserve
     * both old buffers if only one succeeds and moves its allocation. */
    text_run *r = malloc((size_t)cap * sizeof *r);
    /* int8_t and FriBidiLevel (signed char) are binary-compatible; this
     * buffer is handed to fribidi via a cast at the reorder call. */
    int8_t *l = malloc((size_t)cap * sizeof *l);
    if (!r || !l) {
        free(r);
        free(l);
        return false;
    }
    if (t->runs_cap > 0) {
        memcpy(r, t->runs_buf, (size_t)t->runs_cap * sizeof *r);
        memcpy(l, t->run_levels_buf, (size_t)t->runs_cap * sizeof *l);
    }
    free(t->runs_buf);
    free(t->run_levels_buf);
    t->runs_buf = r;
    t->run_levels_buf = l;
    t->runs_cap = cap;
    return true;
}

/* Grow the four itemizer codepoint arrays (cp/bo/bt/lv) to at least
 * `need` entries. Same transactional high-water policy as runs_reserve:
 * either all four move to the new capacity or none do, and the old
 * buffers are left untouched on failure. The sizes are per-entry: a
 * uint32_t offset, a codepoint, a bidi type and a level — ~13 B per
 * input byte in the worst case, bounded by TXT_MAX_INPUT_BYTES at the
 * txt_itemize door. */
static bool cp_scratch_reserve(flux_text *t, int need) {
    if (need <= t->cp_cap)
        return true;
    int cap = t->cp_cap ? t->cp_cap : 256;
    while (cap < need) {
        if (cap > INT_MAX / 2)
            return false;
        cap *= 2;
    }

    FriBidiChar *cp = malloc((size_t)cap * sizeof *cp);
    uint32_t *bo = malloc((size_t)cap * sizeof *bo);
    FriBidiCharType *bt = malloc((size_t)cap * sizeof *bt);
    FriBidiLevel *lv = malloc((size_t)cap * sizeof *lv);
    if (!cp || !bo || !bt || !lv) {
        free(cp);
        free(bo);
        free(bt);
        free(lv);
        return false;
    }
    free(t->cp_buf);
    free(t->bo_buf);
    free(t->bt_buf);
    free(t->lv_buf);
    t->cp_buf = cp;
    t->bo_buf = bo;
    t->bt_buf = bt;
    t->lv_buf = lv;
    t->cp_cap = cap;
    return true;
}

/* Release the itemizer scratch (shutdown + flux_text_compact). */
void txt_itemize_release_scratch(flux_text *t) {
    if (!t)
        return;
    free(t->cp_buf);
    free(t->bo_buf);
    free(t->bt_buf);
    free(t->lv_buf);
    t->cp_buf = NULL;
    t->bo_buf = NULL;
    t->bt_buf = NULL;
    t->lv_buf = NULL;
    t->cp_cap = 0;
}

int txt_itemize(flux_text *t, int slot_idx, const char *utf8, size_t len) {
    if (!t || !utf8 || len == 0)
        return 0;
    /* Reject pathological input up front. The four working arrays below are
     * ~13 B/byte at worst; capping at TXT_MAX_INPUT_BYTES keeps the peak
     * transient allocation bounded and turns a would-be OOM-kill into a
     * clean empty-layout return. */
    if (len > TXT_MAX_INPUT_BYTES)
        return 0;

    /* --- Decode to codepoints, tracking each one's source byte offset. --- */
    size_t blen = len;

    /* Grow the four codepoint arrays to blen+1 (sentinel slot) in ONE
     * transactional step: on failure the old buffers are untouched.
     * blen ≤ TXT_MAX_INPUT_BYTES < UINT32_MAX, so offsets fit uint32_t. */
    if (!cp_scratch_reserve(t, (int)blen + 1))
        return 0;
    FriBidiChar *cps = t->cp_buf;
    uint32_t *bos = t->bo_buf;

    int ncp = 0;
    for (size_t b = 0; b < blen;) {
        uint32_t cp;
        int adv = txt_utf8_decode(utf8 + b, blen - b, &cp);
        cps[ncp] = (FriBidiChar)cp;
        bos[ncp] = (uint32_t)b;
        ncp++;
        b += adv;
    }
    bos[ncp] = (uint32_t)blen; /* sentinel: end byte of last codepoint's run */
    if (ncp == 0)
        return 0;

    /* --- FriBidi embedding levels (auto base direction). --- */
    FriBidiCharType *bt = t->bt_buf;
    FriBidiLevel *lv = t->lv_buf;

    fribidi_get_bidi_types(cps, ncp, bt);
    FriBidiParType base = FRIBIDI_PAR_ON; /* auto-detect per paragraph */
    if (fribidi_get_par_embedding_levels(bt, ncp, &base, lv) == 0) {
        for (int i = 0; i < ncp; i++)
            lv[i] = 0; /* on failure: plain LTR */
    }

    /* --- Split into runs in logical order, growing the run list on demand.
     * --- */
    hb_unicode_funcs_t *uf = hb_unicode_funcs_get_default();

    int cur_face = -1;
    FriBidiLevel cur_lvl = -1;
    hb_script_t cur_script = HB_SCRIPT_INVALID;
    int run_start_cp = 0;

    if (!runs_reserve(t, 1))
        return 0;
    int n = 0;

    for (int i = 0; i < ncp; i++) {
        int face = txt_find_face_for_char(t, slot_idx, cps[i]);
        FriBidiLevel lvl = lv[i];
        hb_script_t sc = resolve_script(uf, cps[i], cur_script);

        bool brk = (cur_face != -1) &&
                   (face != cur_face || lvl != cur_lvl ||
                    (sc != cur_script && sc != HB_SCRIPT_COMMON && sc != HB_SCRIPT_INHERITED));
        if (brk) {
            if (!runs_reserve(t, n + 1))
                return n; /* partial result; caller sees what fit */
            uint32_t bo = bos[run_start_cp];
            t->runs_buf[n] = (text_run){
                .text = utf8 + bo,
                .byte_off = bo,
                .len = bos[i] - bo,
                .slot_idx = slot_idx,
                .face_idx = cur_face,
                .rtl = (cur_lvl & 1) != 0,
                .script = cur_script,
            };
            t->run_levels_buf[n] = cur_lvl;
            n++;
            run_start_cp = i;
        }
        cur_face = face;
        cur_lvl = lvl;
        cur_script = sc;
    }
    /* Flush the trailing run. Unlike the old MAX_RUNS=64 path this never
     * silently drops characters — runs_buf grows to fit whatever the input
     * needs. */
    if (cur_face != -1) {
        if (!runs_reserve(t, n + 1))
            return n;
        uint32_t bo = bos[run_start_cp];
        t->runs_buf[n] = (text_run){
            .text = utf8 + bo,
            .byte_off = bo,
            .len = blen - bo,
            .slot_idx = slot_idx,
            .face_idx = cur_face,
            .rtl = (cur_lvl & 1) != 0,
            .script = cur_script,
        };
        t->run_levels_buf[n] = cur_lvl;
        n++;
    }

    reorder_runs_visual(t->runs_buf, (const FriBidiLevel *)t->run_levels_buf, n);
    return n;
}
