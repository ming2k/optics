/* itemize.c — split a UTF-8 string into runs uniform in direction, script and
 * font face, emitted in visual (left-to-right) order.
 *
 * Pipeline: decode to codepoints -> FriBidi embedding levels -> split on
 * (level, script, face) boundaries in logical order -> reorder the run list to
 * visual order (UBA rule L2). HarfBuzz then shapes each run with the right
 * direction/script, and the layout pass advances a single L->R pen across the
 * visually-ordered runs.
 *
 * Working-set policy: the four transient codepoint arrays (cps, bos, bidi
 * types, levels) live on the stack for short input and fall back to the heap
 * past ITEMIZE_STACK_CP bytes/codepoints. The run list and per-run levels are
 * written into t->runs_buf / t->run_levels_buf, which grow on demand and have
 * no MAX_RUNS cap — a previous hard cap of 64 used to silently drop trailing
 * codepoints past the 64th script/face transition. bos uses uint32_t (not
 * size_t) since strings > 4 GiB are rejected at the door, halving the
 * per-byte working set. */

#include "text_internal.h"

#include <fribidi/fribidi.h>

#include <stdint.h>
#include <stdlib.h>

#define ITEMIZE_STACK_CP 256

/* Resolve a per-codepoint script, letting Common/Inherited extend the run. */
static hb_script_t resolve_script(hb_unicode_funcs_t *uf, FcChar32 cp, hb_script_t cur) {
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
    while (cap < need)
        cap *= 2;
    text_run *r = realloc(t->runs_buf, (size_t)cap * sizeof *r);
    /* int8_t and FriBidiLevel (signed char) are binary-compatible; this
     * buffer is handed to fribidi via a cast at the reorder call. */
    int8_t *l = realloc(t->run_levels_buf, (size_t)cap * sizeof *l);
    if (!r || !l) {
        free(r);
        free(l);
        return false;
    }
    t->runs_buf = r;
    t->run_levels_buf = l;
    t->runs_cap = cap;
    return true;
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

    FriBidiChar stack_cp[ITEMIZE_STACK_CP];
    uint32_t stack_bo[ITEMIZE_STACK_CP];
    FriBidiChar *cps = stack_cp;
    uint32_t *bos = stack_bo;
    bool heap = false;
    if (blen + 1 > ITEMIZE_STACK_CP) {
        /* blen ≤ TXT_MAX_INPUT_BYTES < UINT32_MAX, so (blen+1) fits in
         * size_t without overflow and the offsets fit in uint32_t. */
        cps = malloc((blen + 1) * sizeof *cps);
        bos = malloc((blen + 1) * sizeof *bos);
        if (!cps || !bos) {
            free(cps);
            free(bos);
            return 0;
        }
        heap = true;
    }

    int ncp = 0;
    for (size_t b = 0; b < blen;) {
        FcChar32 cp;
        int adv = txt_utf8_decode(utf8 + b, blen - b, &cp);
        cps[ncp] = (FriBidiChar)cp;
        bos[ncp] = (uint32_t)b;
        ncp++;
        b += adv;
    }
    bos[ncp] = (uint32_t)blen; /* sentinel: end byte of last codepoint's run */
    if (ncp == 0) {
        if (heap) {
            free(cps);
            free(bos);
        }
        return 0;
    }

    /* --- FriBidi embedding levels (auto base direction). --- */
    FriBidiCharType stack_bt[ITEMIZE_STACK_CP];
    FriBidiLevel stack_lv[ITEMIZE_STACK_CP];
    FriBidiCharType *bt = stack_bt;
    FriBidiLevel *lv = stack_lv;
    bool heap2 = false;
    if ((size_t)ncp > ITEMIZE_STACK_CP) {
        bt = malloc((size_t)ncp * sizeof *bt);
        lv = malloc((size_t)ncp * sizeof *lv);
        if (!bt || !lv) {
            free(bt);
            free(lv);
            if (heap) {
                free(cps);
                free(bos);
            }
            return 0;
        }
        heap2 = true;
    }

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

    if (!runs_reserve(t, 1)) {
        if (heap2) {
            free(bt);
            free(lv);
        }
        if (heap) {
            free(cps);
            free(bos);
        }
        return 0;
    }
    int n = 0;

    for (int i = 0; i < ncp; i++) {
        int face = txt_find_face_for_char(t, slot_idx, cps[i]);
        FriBidiLevel lvl = lv[i];
        hb_script_t sc = resolve_script(uf, cps[i], cur_script);

        bool brk = (cur_face != -1) &&
                   (face != cur_face || lvl != cur_lvl ||
                    (sc != cur_script && sc != HB_SCRIPT_COMMON && sc != HB_SCRIPT_INHERITED));
        if (brk) {
            if (!runs_reserve(t, n + 1)) {
                if (heap2) {
                    free(bt);
                    free(lv);
                }
                if (heap) {
                    free(cps);
                    free(bos);
                }
                return n; /* partial result; caller sees what fit */
            }
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
        if (!runs_reserve(t, n + 1)) {
            if (heap2) {
                free(bt);
                free(lv);
            }
            if (heap) {
                free(cps);
                free(bos);
            }
            return n;
        }
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

    if (heap2) {
        free(bt);
        free(lv);
    }
    if (heap) {
        free(cps);
        free(bos);
    }
    return n;
}
