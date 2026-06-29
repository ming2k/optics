//! Layer-1 text layout (line wrapping) on top of the flux-text Layer-0 shaper.
//!
//! The **flux-text** sibling — mirrored by [`flux_text`] — deliberately stops
//! at single-run shaping. The "API layers" note in `<flux-text/text.h>`
//! draws the line explicitly:
//!
//! - **Layer 0** (implemented): shape / measure / draw one contiguous run of
//!   one style. "Sufficient for labels and for callers that do their own
//!   line/paragraph composition."
//! - **Layer 1** (reserved `flux_text_layout`, "Not yet defined"): a
//!   retained, cached layout object built from multiple styled runs with
//!   line wrapping.
//!
//! This crate is a pure-Rust Layer-1 helper: it builds greedy word-wrap on
//! top of [`flux_text::Text::measure`], so the engine boundary stays clean
//! while consumers (and a future C `flux_text_layout`) have something to use
//! today. Keeping it out of the `flux-text` crate means the sibling's Rust
//! surface mirrors only Layer-0 — paragraph composition does not become part
//! of flux-text's API contract until the engine actually defines Layer-1.
//!
//! Scope: single-style line wrapping with `\n` hard breaks and CJK
//! per-character break opportunities. No UAX#14 break iteration, no multi-run
//! paragraphs, no justification — those belong in a fuller layout engine
//! (lens, or the future `flux_text_layout`).

#![deny(rust_2018_idioms)]

use flux_text::{Metrics, Style, Text};

/// Inter-script auto-space, in EM units. Mirrors the same gap the flux-text
/// shaper inserts at CJK↔non-CJK run boundaries (see `txt_run_autospace_em`
/// in libflux's `text/layout.c`): a ¼-em breath so "你好hello" does not render
/// glued. Standard CJK typography (CSS `text-autospace`, ctex/xeCJK) treats
/// this as the default. `wrap` accounts for it so its line widths match what
/// the shaper actually produces when the line is later drawn.
const INTERSCRIPT_GAP_EM: f32 = 0.25;

/// One visual line produced by [`wrap`]. The byte range `[lo, hi)` is in the
/// original input's coordinates; `metrics` is the shaped extent of that
/// substring under the style passed to `wrap`. Lines tile the input exactly
/// once: `lines[i].hi == lines[i+1].lo` and the final `hi` is the input
/// length (or the end of the last non-whitespace run).
///
/// Callers draw each line with [`Text::draw`] using `&text[line.lo..line.hi]`
/// and the original style, advancing `y` by their chosen line stride between
/// lines. Caret / hit-test mapping uses [`Text::x_for_byte`] /
/// [`Text::byte_for_x`] on the same substring, with the byte offset first
/// rebased into the line's local coordinates.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct WrappedLine {
    pub lo: usize,
    pub hi: usize,
    pub metrics: Metrics,
}

/// Greedy word-wrap `input` to fit `max_width` (logical pixels), returning
/// one [`WrappedLine`] per visual line. Each atom is measured as **raw
/// source** at `style` — the convenience for callers whose drawn
/// representation *is* the raw bytes (labels, plain text). Callers whose
/// drawn representation differs from the raw source (hidden inline markup,
/// per-run weight/family) should use [`wrap_with`] and supply the drawn-width
/// measure themselves.
pub fn wrap(text: &Text, input: &str, style: &Style, max_width: f32) -> Vec<WrappedLine> {
    wrap_with(text, input, style, max_width, |r| {
        text.measure(&input[r], style).width
    })
}

/// Like [`wrap`], but each atom's width is supplied by `measure` rather than
/// shaped from the raw source. `measure(byte_range)` returns the drawn width
/// of `&input[byte_range]`, letting the caller account for a representation
/// that differs from the raw bytes — inline markup whose markers are hidden,
/// marks that change weight or family, … This keeps the Layer-1 crate free of
/// any markup model: it stays a pure line-breaker, and all width-affecting
/// representation choices live in the caller. It is also the per-atom cost
/// hook a future optimal-breaker (Knuth-Plass) will consume.
///
/// The algorithm is greedy first-fit, built **on top of** the Layer-0 shaper
/// `text`: `measure` is invoked once per atom (O(n)), and per-line widths are
/// arithmetic over those widths. (An earlier prefix-remeasure implementation
/// was O(n²) in atoms per line; the shaper's glyph cache let it survive, but
/// a few-thousand-atom line made it visibly laggy.)
///
/// There is no UAX#14 break-iterator; break opportunities are:
///
/// - after each whitespace run (the whitespace attaches to the preceding
///   line and is dropped when starting a new line),
/// - between adjacent CJK ideographs (CJK text wraps without spaces),
/// - at every `\n` (treated as a forced hard break),
/// - **last resort:** if a single atom is wider than `max_width`, it is
///   placed on its own line and overflows — the shaper cannot split a single
///   glyph cluster. Long URLs / unbreakable strings will visually extend
///   past `max_width`.
///
/// Empty input returns a single line at `(0, 0)` so callers can still
/// position a caret. Whitespace-only / newline-only input likewise returns
/// one empty line covering `[0, 0)`.
///
/// Width approximation: each inter-atom whitespace byte contributes the
/// measured width of a single ASCII space (`base`). Tab and multi-space runs
/// are therefore treated as N spaces. This matches typical editor behaviour
/// (where tabs already render as a fixed number of spaces) and stays inside
/// the Layer-1 "no full BiDi across the whole line" budget.
///
/// Inter-script auto-space: at a CJK↔non-CJK atom boundary with no
/// separator, a ¼-em gap ([`INTERSCRIPT_GAP_EM`]) is added to mirror the
/// flux-text shaper, which inserts the same gap when the line is drawn.
/// Without it wrap would under-count and pack a line that overflows on
/// screen. A boundary already covered by whitespace is left alone (no
/// doubling).
pub fn wrap_with(
    text: &Text,
    input: &str,
    base: &Style,
    max_width: f32,
    measure: impl Fn(std::ops::Range<usize>) -> f32,
) -> Vec<WrappedLine> {
    if input.is_empty() {
        return vec![WrappedLine {
            lo: 0,
            hi: 0,
            metrics: text.measure("", base),
        }];
    }

    let atoms = split_atoms(input);
    if atoms.is_empty() {
        return vec![WrappedLine {
            lo: 0,
            hi: 0,
            metrics: text.measure("", base),
        }];
    }

    // Pre-measure every atom once via the caller-supplied measure. This is
    // the only per-atom shaping the algorithm does; everything below is
    // arithmetic over those widths.
    let atom_w: Vec<f32> = atoms.iter().map(|a| measure(a.start..a.end)).collect();

    // Kinsoku classes per atom: whether it must not begin a line (no_start)
    // or must not end one (no_end). A soft break between atoms idx-1 and idx
    // is legal only when atoms[idx] may begin a line and atoms[idx-1] may
    // end one; otherwise the break is forbidden and the atom is force-fitted.
    let no_start: Vec<bool> = atoms.iter().map(|&a| atom_no_start(input, a)).collect();
    let no_end: Vec<bool> = atoms.iter().map(|&a| atom_no_end(input, a)).collect();

    // Width of one ASCII space — used to approximate the cost of the
    // whitespace run between two adjacent atoms. Measuring each actual
    // inter-atom whitespace run separately would re-introduce O(n²).
    // Tab and multi-space runs collapse to N spaces, which matches how
    // most editors and Layer-1 widgets render them anyway.
    let space_w = text.measure(" ", base).width.max(0.0);

    // Line metrics: height/baseline come from a base-style shape (font
    // metrics, unaffected by marks); width is the caller's drawn width for
    // the line's source range, so the returned metric matches what gets
    // painted.
    let line_metrics = |lo: usize, hi: usize| {
        let mut m = text.measure(&input[lo..hi], base);
        m.width = measure(lo..hi);
        m
    };

    let mut lines: Vec<WrappedLine> = Vec::new();

    // Index of the first atom on the current line, plus the byte end of
    // the last atom that fit. The first atom on a line is always placed
    // unconditionally (even if it overflows) so the loop terminates on a
    // single atom wider than max_width.
    let mut line_first = 0usize;
    let mut line_w = atom_w[0];
    let mut line_last_end = atoms[0].end;

    let mut idx = 1usize;
    while idx < atoms.len() {
        // The gap between atoms idx-1 and idx (whitespace width + the
        // shaper-mirrored inter-script auto-space), and whether it is a
        // forced `\n` break. Shared with the optimal strategy via
        // `gap_cost` so the inter-script rule has one source of truth.
        let (gap_w, hard_break) = gap_cost(input, &atoms, idx, space_w, base.size_px);

        if hard_break {
            // `\n` forces a line break regardless of width.
            let lo = atoms[line_first].start;
            let hi = line_last_end;
            lines.push(WrappedLine {
                lo,
                hi,
                metrics: line_metrics(lo, hi),
            });
            line_first = idx;
            line_w = atom_w[idx];
            line_last_end = atoms[idx].end;
        } else if line_w + gap_w + atom_w[idx] <= max_width || no_start[idx] || no_end[idx - 1] {
            // Extend the line — either the atom fits, or kinsoku forbids
            // breaking here: atoms[idx] is a no-start char (。，、）》」…)
            // that must not begin a line, or atoms[idx-1] is a no-end
            // opening bracket that must not end one. In the kinsoku case
            // the line overflows rather than stranding punctuation at a
            // line edge.
            line_w += gap_w + atom_w[idx];
            line_last_end = atoms[idx].end;
        } else {
            // Overflow, and breaking here is kinsoku-legal — close the
            // current line and start a new one at this atom. Whitespace
            // before the new line is dropped.
            let lo = atoms[line_first].start;
            let hi = line_last_end;
            lines.push(WrappedLine {
                lo,
                hi,
                metrics: line_metrics(lo, hi),
            });
            line_first = idx;
            line_w = atom_w[idx];
            line_last_end = atoms[idx].end;
        }
        idx += 1;
    }

    // Finalize the trailing line.
    let lo = atoms[line_first].start;
    let hi = line_last_end;
    lines.push(WrappedLine {
        lo,
        hi,
        metrics: line_metrics(lo, hi),
    });

    lines
}

// ----------------------------------------------------------------------
// Knuth-Plass optimal line breaking
// ----------------------------------------------------------------------
//
// The full paragraph-level DP from Knuth & Plass (*Digital Typography*):
// minimise total demerit = Σ badness(line) + Σ adjacent-class demerit,
// where each line is classified into a fitness class by how far it falls
// short of `max_width`. The class term is what makes a K-P paragraph look
// *even* — it penalises a tight line sitting next to a loose one — and is
// the part a plain sum-of-shortfalls DP (which collapses to "fill each
// line", ≈ greedy) misses.
//
// It consumes the same caller-supplied `measure` and the same `gap_cost`
// rule as the greedy path, so it stays consistent with the shaper and with
// the drawn representation. For ragged-right body text its edge over greedy
// is real but subtle (most visible on dense CJK, where per-character break
// opportunities multiply, and where greedy would strand a short final
// line); it pays off fully once justification is in scope.

/// Knuth-Plass optimal wrap — like [`wrap_with`] but choosing break points
/// to minimise whole-paragraph demerit (badness + fitness-class evenness)
/// rather than filling each line greedily. Same `measure` contract, same
/// [`WrappedLine`] output, so it is a drop-in replacement for [`wrap_with`].
/// See the "Knuth-Plass optimal line breaking" notes above for when it pays
/// off. Each atom is measured as raw source at `style`; callers whose drawn
/// representation differs from the raw bytes should use
/// [`wrap_optimal_with`].
pub fn wrap_optimal(text: &Text, input: &str, style: &Style, max_width: f32) -> Vec<WrappedLine> {
    wrap_optimal_with(text, input, style, max_width, |r| {
        text.measure(&input[r], style).width
    })
}

/// Knuth-Plass optimal wrap with a caller-supplied measure. See
/// [`wrap_with`] for the `measure` contract and [`wrap_optimal`] for the
/// strategy. Falls back to [`wrap_with`] (greedy) for inputs above
/// [`KP_GREEDY_FALLBACK_ATOMS`] atoms, where the O(n²·C) DP would risk
/// dropping a frame on a megabyte paste.
pub fn wrap_optimal_with(
    text: &Text,
    input: &str,
    base: &Style,
    max_width: f32,
    measure: impl Fn(std::ops::Range<usize>) -> f32,
) -> Vec<WrappedLine> {
    if input.is_empty() {
        return vec![WrappedLine {
            lo: 0,
            hi: 0,
            metrics: text.measure("", base),
        }];
    }
    let atoms = split_atoms(input);
    if atoms.is_empty() {
        return vec![WrappedLine {
            lo: 0,
            hi: 0,
            metrics: text.measure("", base),
        }];
    }
    if atoms.len() > KP_GREEDY_FALLBACK_ATOMS {
        return wrap_with(text, input, base, max_width, measure);
    }

    let em = base.size_px.max(1.0);
    let space_w = text.measure(" ", base).width.max(0.0);

    let atom_w: Vec<f32> = atoms.iter().map(|a| measure(a.start..a.end)).collect();
    // Kinsoku classes (see `wrap_with`). The DP only places line-ends at
    // break points kinsoku allows, so an optimal solution never strands a
    // no-start char at a line start or a no-end bracket at a line end.
    let no_start: Vec<bool> = atoms.iter().map(|&a| atom_no_start(input, a)).collect();
    let no_end: Vec<bool> = atoms.iter().map(|&a| atom_no_end(input, a)).collect();
    let mut gap_w = Vec::with_capacity(atoms.len() - 1);
    let mut gap_hard = Vec::with_capacity(atoms.len() - 1);
    for k in 0..atoms.len() - 1 {
        let (w, hard) = gap_cost(input, &atoms, k + 1, space_w, base.size_px);
        gap_w.push(w);
        gap_hard.push(hard);
    }

    // Prefix sums for O(1) line-width lookups. PA[k] = Σ atom_w[0..k];
    // PG[k] = Σ gap_w[0..k] (gap_w[k] sits between atom k and k+1, so it
    // belongs to any line containing both). natural(i,j) for atoms i..j and
    // the gaps between them = (PA[j]-PA[i]) + (PG[j-1]-PG[i]).
    let mut pa = vec![0.0f32; atoms.len() + 1];
    for k in 0..atoms.len() {
        pa[k + 1] = pa[k] + atom_w[k];
    }
    let mut pg = vec![0.0f32; atoms.len()];
    for k in 0..gap_w.len() {
        pg[k + 1] = pg[k] + gap_w[k];
    }
    let natural = |i: usize, j: usize| -> f32 {
        let g = if j > 0 { pg[j - 1] - pg[i] } else { 0.0 };
        (pa[j] - pa[i]) + g
    };

    let line_metrics = |lo: usize, hi: usize| {
        let mut m = text.measure(&input[lo..hi], base);
        m.width = measure(lo..hi);
        m
    };

    // Run the DP independently over each run of atoms between forced (`\n`)
    // breaks; each segment's final line gets the \parfillskip exemption.
    let mut out: Vec<WrappedLine> = Vec::new();
    let mut seg_start = 0usize;
    while seg_start < atoms.len() {
        let mut seg_end = seg_start;
        while seg_end + 1 < atoms.len() && !gap_hard[seg_end] {
            seg_end += 1;
        }
        // Segment = atoms[seg_start..=seg_end]; `na` atoms, local idx 0..=na-1.
        let na = seg_end - seg_start + 1;
        knuth_plass_segment(
            seg_start,
            na,
            &atoms,
            &natural,
            &no_start,
            &no_end,
            em,
            max_width,
            &line_metrics,
            &mut out,
        );
        seg_start = seg_end + 1;
    }

    out
}

/// Run the K-P DP over one hard-break-free segment of `na` atoms starting at
/// global atom index `seg_start`, appending the resulting lines to `out`.
///
/// State `dp[j][c]` = best (cost, back_i, back_c) to lay out the first `j`
/// local atoms such that the line ending at `j` is in fitness class `c`.
/// `j` ranges `0..=na`; `c` ranges `0..NCLASS`. The class discretisation is
/// what makes the DP finite — the continuous shortfall is carried only
/// implicitly through the class.
fn knuth_plass_segment(
    seg_start: usize,
    na: usize,
    atoms: &[Atom],
    natural: &impl Fn(usize, usize) -> f32,
    no_start: &[bool],
    no_end: &[bool],
    em: f32,
    max_width: f32,
    line_metrics: &impl Fn(usize, usize) -> Metrics,
    out: &mut Vec<WrappedLine>,
) {
    const NCLASS: usize = 4;
    let inf = f32::INFINITY;
    // dp[0] is the source: zero cost, no predecessor. Its four entries are
    // identical; transitions treat i == 0 specially (no evenness demerit).
    let mut dp: Vec<[(f32, usize, u8); NCLASS]> = vec![[(inf, 0, 0); NCLASS]; na + 1];
    dp[0] = [(0.0, 0, 0); NCLASS];

    // Scan i backwards from j-1: as i shrinks the line gains atoms, so width
    // rises and shortfall falls. Overfull multi-atom lines let us bail early
    // (smaller i only adds width); too-loose lines just get skipped (smaller
    // i may bring the line back under the looseness ceiling).
    for j in 1..=na {
        // Kinsoku: a line may end at j only if atom j-1 may end a line and
        // (unless j is the segment's final line) atom j may begin one.
        // Leaving dp[j] at INF makes the DP span over j — the atoms around
        // j join a longer line that ends at a legal break point.
        if j < na && (no_end[seg_start + j - 1] || no_start[seg_start + j]) {
            continue;
        }
        for i in (0..j).rev() {
            let nat = natural(seg_start + i, seg_start + j);
            let single = j - i == 1;
            let is_last = j == na;
            if nat > max_width && !single {
                break;
            }
            let shortfall_em = ((max_width - nat).max(0.0)) / em;
            // Final line: up to KP_LAST_LINE_FREE_EM of shortfall is free
            // (≈ \parfillskip) — a short last line is correct, not bad.
            let eff_em = if is_last {
                (shortfall_em - KP_LAST_LINE_FREE_EM).max(0.0)
            } else {
                shortfall_em
            };
            if !is_last && !single && shortfall_em > KP_MAX_SHORTFALL_EM {
                continue;
            }
            let class = kp_class(eff_em);
            let badness = eff_em * eff_em * eff_em;

            // Best predecessor over the previous line's class.
            let (mut best, mut best_cprev) = (inf, 0u8);
            if i == 0 {
                best = 0.0;
            } else {
                for cprev in 0..NCLASS {
                    let (pcost, _, _) = dp[i][cprev];
                    if pcost >= inf {
                        continue;
                    }
                    let even = if cprev == class {
                        0.0
                    } else {
                        KP_CLASS_DEMERIT
                    };
                    let cost = pcost + even;
                    if cost < best {
                        best = cost;
                        best_cprev = cprev as u8;
                    }
                }
            }
            if best >= inf {
                continue;
            }
            let cand = best + badness;
            if cand < dp[j][class].0 {
                dp[j][class] = (cand, i, best_cprev);
            }
        }
    }

    // Recover the cheapest final state and walk the back-pointers.
    let (mut end_c, mut end_cost) = (0usize, inf);
    for c in 0..NCLASS {
        if dp[na][c].0 < end_cost {
            end_cost = dp[na][c].0;
            end_c = c;
        }
    }
    let mut breaks: Vec<usize> = Vec::with_capacity(na + 1);
    breaks.push(na);
    let (mut cur_j, mut cur_c) = (na, end_c);
    while cur_j > 0 {
        let (_, back_i, back_c) = dp[cur_j][cur_c];
        breaks.push(back_i);
        cur_j = back_i;
        if cur_j > 0 {
            cur_c = back_c as usize;
        }
    }
    breaks.reverse(); // [0, b1, ..., na]

    for w in breaks.windows(2) {
        let (li, lj) = (w[0], w[1]);
        let glo = atoms[seg_start + li].start;
        let ghi = atoms[seg_start + lj - 1].end;
        out.push(WrappedLine {
            lo: glo,
            hi: ghi,
            metrics: line_metrics(glo, ghi),
        });
    }
}

/// Fitness class 0..3 for a line whose effective shortfall is `eff_em` EM
/// (tight / decent / loose / very-loose). Used by [`knuth_plass_segment`].
fn kp_class(eff_em: f32) -> usize {
    if eff_em < KP_FITNESS_EM[0] {
        0
    } else if eff_em < KP_FITNESS_EM[1] {
        1
    } else if eff_em < KP_FITNESS_EM[2] {
        2
    } else {
        3
    }
}

/// Fitness-class boundaries in EM of shortfall (max_width − natural width).
/// Four classes (the standard K-P count) — discretising shortfall into
/// classes is what keeps the DP finite: the state carries the previous
/// line's class, not its continuous shortfall.
const KP_FITNESS_EM: [f32; 3] = [0.5, 1.5, 3.0];

/// Demerit added when two adjacent lines land in different fitness classes.
/// Scaled to be comparable to a line's badness (≈ shortfall_em³), so the
/// optimiser trades line badness against evenness rather than one
/// dominating. Tunable.
const KP_CLASS_DEMERIT: f32 = 10.0;

/// A non-final, non-single-atom line whose shortfall exceeds this (in EM) is
/// "too loose" to consider — rules out absurd breaks and bounds the active
/// set. Single-atom lines and each segment's final line are exempt.
const KP_MAX_SHORTFALL_EM: f32 = 6.0;

/// Free shortfall granted to a segment's final line (≈ TeX's
/// `\parfillskip`): a short last line is correct, not bad, so up to this
/// much costs nothing. Beyond it the line is penalised normally — which is
/// what discourages stranding a single word on the final line.
const KP_LAST_LINE_FREE_EM: f32 = 2.0;

/// Atom count above which K-P falls back to greedy. K-P is O(n²·C)
/// (C = [`kp_class`]'s 4 classes); past this a megabyte paste could drop a
/// frame. Well beyond any realistic paragraph.
const KP_GREEDY_FALLBACK_ATOMS: usize = 3000;

/// A maximal run of source bytes that the wrap algorithm treats as atomic
/// (cannot be split). See [`wrap`].
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct Atom {
    start: usize,
    end: usize,
}

/// Split `input` into wrap atoms. Whitespace and `\n` are *separators* (not
/// atoms); they define break opportunities between atoms. Atoms are:
///
/// - a maximal run of non-whitespace, non-CJK characters (a Latin "word",
///   including digits, punctuation, emoji ZWJ sequences by codepoint), or
/// - a single CJK ideograph (which is its own atom so CJK text wraps without
///   spaces),
///
/// Adjacent atoms imply a break opportunity between them.
fn split_atoms(input: &str) -> Vec<Atom> {
    let mut atoms = Vec::new();
    let mut chars = input.char_indices().peekable();

    while let Some((i, ch)) = chars.next() {
        if ch == '\n' || ch.is_whitespace() {
            // Separator — skip. Whitespace runs collapse together because
            // each whitespace char takes this branch.
            continue;
        }

        let start = i;
        if is_cjk(ch) {
            // Single CJK char is its own atom.
            atoms.push(Atom {
                start,
                end: i + ch.len_utf8(),
            });
        } else {
            // Read a maximal run of non-whitespace, non-CJK chars.
            let mut end = i + ch.len_utf8();
            while let Some(&(_, next_ch)) = chars.peek() {
                if next_ch == '\n' || next_ch.is_whitespace() || is_cjk(next_ch) {
                    break;
                }
                end += next_ch.len_utf8();
                chars.next();
            }
            atoms.push(Atom { start, end });
        }
    }

    atoms
}

/// Whether `ch` is a CJK ideograph or related script where line-wrap should
/// be allowed at every character. Covers CJK Unified, CJK Ext A/B/C/D/E/F/G,
/// Hiragana, Katakana, CJK symbols/punctuation, Hangul, and full-width
/// forms. Not exhaustive, but covers the common case: text in CJK locales
/// wraps character-by-character rather than at whitespace.
fn is_cjk(ch: char) -> bool {
    matches!(ch as u32,
        0x1100..=0x11FF   // Hangul Jamo
        | 0x2E80..=0x2EFF // CJK Radicals Supplement
        | 0x2F00..=0x2FDF // Kangxi Radicals
        | 0x3000..=0x303F // CJK Symbols and Punctuation
        | 0x3040..=0x309F // Hiragana
        | 0x30A0..=0x30FF // Katakana
        | 0x3100..=0x312F // Bopomofo
        | 0x3130..=0x318F // Hangul Compatibility Jamo
        | 0x3400..=0x4DBF // CJK Unified Ideographs Extension A
        | 0x4E00..=0x9FFF // CJK Unified Ideographs
        | 0xA960..=0xA97F // Hangul Jamo Extended-A
        | 0xAC00..=0xD7AF // Hangul Syllables
        | 0xD7B0..=0xD7FF // Hangul Jamo Extended-B
        | 0xF900..=0xFAFF // CJK Compatibility Ideographs
        | 0xFF00..=0xFFEF // Halfwidth and Full-width Forms
        | 0x1F300..=0x1FAFF // Symbols & Pictographs + CJK symbols (broad emoji)
        | 0x20000..=0x2A6DF // CJK Unified Ideographs Extension B
        | 0x2A700..=0x2B73F // CJK Ext C
        | 0x2B740..=0x2B81F // CJK Ext D
        | 0x2B820..=0x2CEAF // CJK Ext E
        | 0x2CEB0..=0x2EBEF // CJK Ext F
        | 0x30000..=0x3134F // CJK Ext G
    )
}

/// Whether `atom`'s first codepoint is CJK — the atom's script class for the
/// inter-script auto-space rule in [`wrap`]. Derived from the text rather
/// than stored on [`Atom`], so the struct stays minimal and the rule reads
/// its input from the single source of truth (the codepoint), matching how
/// the C shaper derives the same fact from `run->script`.
fn atom_is_cjk(input: &str, atom: Atom) -> bool {
    input[atom.start..].chars().next().map_or(false, is_cjk)
}

/// The `(width, is_hard_break)` of the gap between atoms `idx - 1` and
/// `idx` — i.e. the source bytes `atoms[idx-1].end .. atoms[idx].start`.
///
/// The width includes the inter-script auto-space when the two atoms cross
/// the CJK line and no separator already covers the boundary. This is the
/// **single source of truth** for that rule: both the greedy
/// ([`wrap_with`]) and optimal ([`wrap_optimal_with`]) strategies call it,
/// so neither can drift from the ¼-em gap the shaper inserts.
fn gap_cost(input: &str, atoms: &[Atom], idx: usize, space_w: f32, size_px: f32) -> (f32, bool) {
    let gap = &input[atoms[idx - 1].end..atoms[idx].start];
    let hard = gap.contains('\n');
    let mut w = space_w * gap.bytes().filter(|&b| b != b'\n').count() as f32;
    // `is_cjk` is a superset of the shaper's script test (it also covers
    // emoji / full-width), so this only ever over-estimates relative to the
    // shaper — the safe direction for line-break accounting.
    if w == 0.0 && atom_is_cjk(input, atoms[idx - 1]) != atom_is_cjk(input, atoms[idx]) {
        w += size_px * INTERSCRIPT_GAP_EM;
    }
    (w, hard)
}

/// CJK punctuation that must not **begin** a line: closing brackets and
/// stop punctuation (。，、）》」…). JLREQ cl-02 / cl-06 / cl-07. A break
/// immediately before one would strand it at a line start, so kinsoku shori
/// forbids such breaks (the char is pulled back onto the previous line).
fn is_no_start(ch: char) -> bool {
    matches!(
        ch,
        '〉' | '》'
            | '」'
            | '』'
            | '】'
            | '〕'
            | '〗'
            | '〙'
            | '〟'
            | '）'
            | '］'
            | '｝'
            | '｠'
            | '、'
            | '。'
            | '，'
            | '．'
            | '：'
            | '；'
            | '！'
            | '？'
    )
}

/// CJK opening brackets that must not **end** a line: （（「『…. JLREQ
/// cl-01. A break immediately after one would leave it dangling at a line
/// end, separated from what it opens, so kinsoku forbids such breaks (the
/// bracket is pushed onto the next line).
fn is_no_end(ch: char) -> bool {
    matches!(
        ch,
        '〈' | '《' | '「' | '『' | '【' | '〔' | '〖' | '〚' | '〝' | '（' | '［' | '｛' | '｟'
    )
}

/// Whether `atom`'s first character is no-start (must not begin a line).
fn atom_no_start(input: &str, atom: Atom) -> bool {
    input[atom.start..]
        .chars()
        .next()
        .map_or(false, is_no_start)
}

/// Whether `atom`'s last character is no-end (must not end a line).
fn atom_no_end(input: &str, atom: Atom) -> bool {
    input[..atom.end].chars().last().map_or(false, is_no_end)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Build a measure-only shaper for wrap tests. Returns `None` if the
    /// engine cannot be initialised (e.g. no fontconfig in CI), in which
    /// case engine-dependent tests skip themselves.
    fn engine() -> Option<Text> {
        Text::measure_only().ok()
    }

    fn style() -> Style {
        Style::new(17.0, 0xFFFFFFFF)
    }

    #[test]
    #[ignore = "verification smoke-print; run with --nocapture --ignored"]
    fn kp_smoke_realistic() {
        let Some(engine) = engine() else { return };
        let st = style();
        let para = "Knuth-Plass \u{6574}\u{6bb5}\u{6700}\u{4f18}\u{65ad}\u{884c}\u{7b97}\u{6cd5}\u{901a}\u{8fc7}\u{52a8}\u{6001}\u{89c4}\u{5212}\u{6700}\u{5c0f}\u{5316}\u{6574}\u{6bb5}\u{7684}\u{6392}\u{7248}\u{4ee3}\u{4ef7}\u{ff0c}\u{800c}\u{4e0d}\u{662f}\u{50cf}\u{8d2a}\u{5fc3}\u{7b97}\u{6cd5}\u{90a3}\u{6837}\u{9010}\u{884c}\u{586b}\u{5145}\u{3002}\u{5b83}\u{7684}\u{6838}\u{5fc3}\u{662f}\u{628a}\u{6bcf}\u{4e2a}\u{53ef}\u{884c}\u{7684}\u{65ad}\u{70b9}\u{4f5c}\u{4e3a}\u{4e00}\u{4e2a}\u{72b6}\u{6001}\u{ff0c}\u{7528} badness \u{8861}\u{91cf}\u{6bcf}\u{4e00}\u{884c}\u{7684}\u{677e}\u{7d27}\u{ff0c}\u{5e76}\u{60e9}\u{7f5a}\u{76f8}\u{90bb}\u{884c}\u{4e4b}\u{95f4}\u{8fc7}\u{5927}\u{7684}\u{677e}\u{7d27}\u{5dee}\u{5f02}\u{3002}对于中文混排，断行的选择空间更大，整段最优的价值也更明显。";
        let max_w = 420.0;
        let greedy = wrap(&engine, para, &st, max_w);
        let optimal = wrap_optimal(&engine, para, &st, max_w);
        println!(
            "\n=== column {max_w}px, {} chars | greedy {} lines, K-P {} lines ===",
            para.chars().count(),
            greedy.len(),
            optimal.len()
        );
        println!("--- K-P ---");
        for (i, l) in optimal.iter().enumerate() {
            let w = engine.measure(&para[l.lo..l.hi], &st).width;
            println!("  KP {i}: {w:>6.1}px  {:?}", &para[l.lo..l.hi]);
        }
        println!("--- greedy ---");
        for (i, l) in greedy.iter().enumerate() {
            let w = engine.measure(&para[l.lo..l.hi], &st).width;
            println!("  gr {i}: {w:>6.1}px  {:?}", &para[l.lo..l.hi]);
        }
        for l in &optimal {
            let slice = &para[l.lo..l.hi];
            let atoms = super::split_atoms(slice);
            let w = engine.measure(slice, &st).width;
            assert!(
                atoms.len() == 1 || w <= max_w + 0.5,
                "overflow {w} > {max_w}: {slice:?}"
            );
        }
    }

    #[test]
    fn split_atoms_latin_words() {
        let atoms = split_atoms("hello world  foo");
        assert_eq!(
            atoms,
            vec![
                Atom { start: 0, end: 5 },   // "hello"
                Atom { start: 6, end: 11 },  // "world"
                Atom { start: 13, end: 16 }  // "foo"
            ]
        );
    }

    #[test]
    fn split_atoms_cjk_each_char_is_atomic() {
        // Chinese: no whitespace, but each char is its own atom.
        let atoms = split_atoms("你好世界");
        assert_eq!(
            atoms,
            vec![
                Atom { start: 0, end: 3 },  // 你
                Atom { start: 3, end: 6 },  // 好
                Atom { start: 6, end: 9 },  // 世
                Atom { start: 9, end: 12 }  // 界
            ]
        );
    }

    #[test]
    fn split_atoms_mixed_cjk_and_latin() {
        // Mixed: "你好 hello 世界" — atoms split at CJK/Latin boundaries.
        let atoms = split_atoms("你好 hello 世界");
        assert_eq!(
            atoms,
            vec![
                Atom { start: 0, end: 3 },   // 你
                Atom { start: 3, end: 6 },   // 好
                Atom { start: 7, end: 12 },  // hello
                Atom { start: 13, end: 16 }, // 世
                Atom { start: 16, end: 19 }  // 界
            ]
        );
    }

    #[test]
    fn split_atoms_newline_is_separator() {
        let atoms = split_atoms("a\nb\nc");
        assert_eq!(
            atoms,
            vec![
                Atom { start: 0, end: 1 }, // a
                Atom { start: 2, end: 3 }, // b
                Atom { start: 4, end: 5 }  // c
            ]
        );
    }

    #[test]
    fn split_atoms_empty_or_whitespace_only() {
        assert!(split_atoms("").is_empty());
        assert!(split_atoms("   \n\t  ").is_empty());
    }

    #[test]
    fn wrap_empty_input_returns_single_empty_line() {
        let Some(engine) = engine() else { return };
        let st = style();
        let lines = wrap(&engine, "", &st, 100.0);
        assert_eq!(lines.len(), 1);
        assert_eq!(lines[0].lo, 0);
        assert_eq!(lines[0].hi, 0);
    }

    #[test]
    fn wrap_whitespace_only_returns_single_empty_line() {
        let Some(engine) = engine() else { return };
        let st = style();
        let lines = wrap(&engine, "   \n  ", &st, 100.0);
        assert_eq!(lines.len(), 1);
        assert_eq!(lines[0].lo, 0);
        assert_eq!(lines[0].hi, 0);
    }

    #[test]
    fn wrap_single_word_one_line() {
        let Some(engine) = engine() else { return };
        let st = style();
        // A short word always fits in any reasonable max_width.
        let lines = wrap(&engine, "hello", &st, 10000.0);
        assert_eq!(lines.len(), 1);
        assert_eq!(lines[0].lo, 0);
        assert_eq!(lines[0].hi, 5);
    }

    #[test]
    fn wrap_byte_ranges_tile_input_exactly() {
        let Some(engine) = engine() else { return };
        let st = style();
        // Three paragraph styles: short Latin, long CJK (no spaces), mixed.
        // For each, lines must cover all non-whitespace bytes exactly once,
        // with only whitespace between consecutive line ranges.
        for input in [
            "the quick brown fox jumps over the lazy dog",
            "你好世界你好世界你好世界你好世界",
            "mixed 你好 world 世界 with words",
        ] {
            let lines = wrap(&engine, input, &st, 100.0);
            assert!(!lines.is_empty(), "wrap returned no lines for {input:?}");
            assert_eq!(lines[0].lo, 0, "first line should start at 0");
            // Consecutive lines: no overlap, and any gap between them is
            // pure whitespace (the wrap algorithm trims the break).
            for w in lines.windows(2) {
                assert!(
                    w[0].hi <= w[1].lo,
                    "lines must not overlap: {:?} in {input:?}",
                    lines
                );
                let gap = &input[w[0].hi..w[1].lo];
                assert!(
                    gap.bytes().all(|b| b == b' ' || b == b'\n' || b == b'\t'),
                    "gap between lines must be whitespace, found {gap:?} in {input:?}: {lines:?}"
                );
            }
            // Last line's hi must be at or near input end. Trailing whitespace
            // (if any) is dropped from the last line.
            let last = lines.last().unwrap();
            assert!(
                last.hi <= input.len(),
                "last line hi out of range: {} > {}",
                last.hi,
                input.len()
            );
            // No non-whitespace byte should be uncovered.
            let covered = lines
                .iter()
                .flat_map(|l| l.lo..l.hi)
                .collect::<std::collections::HashSet<_>>();
            for (i, b) in input.bytes().enumerate() {
                if b != b' ' && b != b'\n' && b != b'\t' {
                    assert!(
                        covered.contains(&i),
                        "byte {i} ({b}) uncovered in {input:?}: {lines:?}"
                    );
                }
            }
        }
    }

    #[test]
    fn wrap_narrow_column_breaks_between_atoms() {
        let Some(engine) = engine() else { return };
        let st = style();
        // 10 short words; with a narrow column we expect >1 line.
        let text = "the quick brown fox jumps over the lazy dog again";
        let wide = wrap(&engine, text, &st, 10000.0);
        assert_eq!(wide.len(), 1, "very wide column should keep one line");
        let narrow = wrap(&engine, text, &st, 50.0);
        assert!(
            narrow.len() > 1,
            "narrow column should wrap, got {} lines: {:?}",
            narrow.len(),
            narrow
        );
        // Narrow wrap should still cover the input contiguously modulo
        // whitespace gaps at line breaks.
        assert_eq!(narrow[0].lo, 0);
        for w in narrow.windows(2) {
            assert!(w[0].hi <= w[1].lo);
            let gap = &text[w[0].hi..w[1].lo];
            assert!(gap.bytes().all(|b| b == b' ' || b == b'\n' || b == b'\t'));
        }
        assert_eq!(narrow.last().unwrap().hi, text.len());
    }

    #[test]
    fn wrap_long_atom_overflows_gracefully() {
        let Some(engine) = engine() else { return };
        let st = style();
        // A single 200-char word: cannot be split, so wrap emits one line
        // (overflowing), with hi covering the input.
        let long = "a".repeat(200);
        let lines = wrap(&engine, &long, &st, 50.0);
        assert_eq!(lines.len(), 1, "unbreakable atom stays on one line");
        assert_eq!(lines[0].lo, 0);
        assert_eq!(lines[0].hi, long.len());
    }

    #[test]
    fn wrap_optimal_tiles_input_like_greedy() {
        // The optimal path keeps the WrappedLine contract: lines tile the
        // input contiguously modulo whitespace, regardless of strategy.
        let Some(engine) = engine() else { return };
        let st = style();
        for input in [
            "the quick brown fox jumps over the lazy dog again and again",
            "你好世界你好世界你好世界你好世界",
            "mixed 你好 hello 世界 world again 更多文字",
        ] {
            let lines = wrap_optimal(&engine, input, &st, 90.0);
            assert!(!lines.is_empty(), "no lines for {input:?}");
            assert_eq!(lines[0].lo, 0, "first line starts at 0");
            for w in lines.windows(2) {
                assert!(w[0].hi <= w[1].lo, "overlap in {input:?}: {lines:?}");
                let gap = &input[w[0].hi..w[1].lo];
                assert!(
                    gap.bytes().all(|b| b == b' ' || b == b'\n' || b == b'\t'),
                    "non-whitespace gap in {input:?}: {gap:?}"
                );
            }
            assert!(lines.last().unwrap().hi <= input.len());
        }
    }

    #[test]
    fn wrap_optimal_never_overflows_a_multi_atom_line() {
        // No multi-atom line may exceed max_width; only a single unbreakable
        // atom is allowed to overflow. (Feasibility is enforced inside the
        // DP; this guards against a regression.)
        let Some(engine) = engine() else { return };
        let st = style();
        let text = "the quick brown fox jumps over the lazy dog again and again";
        let max_w = 120.0;
        let lines = wrap_optimal(&engine, text, &st, max_w);
        for (k, line) in lines.iter().enumerate() {
            let slice = &text[line.lo..line.hi];
            // Count atoms in this line; if >1, its drawn width must fit.
            let atoms = super::split_atoms(slice);
            if atoms.len() > 1 {
                let w = engine.measure(slice, &st).width;
                assert!(
                    w <= max_w + 0.5,
                    "line {k} ({} atoms, {w:.1}px) overflows {max_w}: {slice:?}",
                    atoms.len()
                );
            }
        }
    }

    #[test]
    fn wrap_optimal_honours_newline_as_forced_break() {
        let Some(engine) = engine() else { return };
        let st = style();
        let lines = wrap_optimal(&engine, "a\nb", &st, 10000.0);
        assert_eq!(lines.len(), 2, "newline forces a break under K-P too");
        assert_eq!(lines[0].lo, 0);
        assert_eq!(lines[0].hi, 1);
        assert_eq!(lines[1].lo, 2);
        assert_eq!(lines[1].hi, 3);
    }

    #[test]
    fn wrap_kinsoku_holds_on_punctuated_paragraph() {
        // Across columns and both strategies, no wrapped line may begin with
        // a no-start char (。，、）》」…) or end with a no-end opening bracket
        // （（《「『…. This is the kinsoku invariant.
        let Some(engine) = engine() else { return };
        let st = style();
        let para = "\u{6d4b}\u{8bd5}\u{ff08}\u{62ec}\u{53f7}\u{ff09}\u{548c}\u{6807}\u{70b9}\u{7b26}\u{53f7}\u{3002}\u{53e6}\u{4e00}\u{53e5}\u{8bdd}\u{ff0c}\u{8fd8}\u{6709}\u{66f4}\u{591a}\u{6587}\u{5b57}\u{ff01}\u{6700}\u{540e}\u{662f}\u{95ee}\u{53f7}\u{ff1f}\u{597d}\u{7684}\u{3002}";
        for max_w in [60.0, 90.0, 140.0, 240.0, 400.0] {
            let greedy = wrap(&engine, para, &st, max_w);
            let optimal = wrap_optimal(&engine, para, &st, max_w);
            for (strategy, lines) in [("greedy", greedy), ("KP", optimal)] {
                for l in &lines {
                    let first = para[l.lo..].chars().next();
                    let last = para[..l.hi].chars().last();
                    assert!(
                        !first.map_or(false, is_no_start),
                        "{strategy} @ {max_w}: line starts with no-start: {:?}",
                        &para[l.lo..l.hi]
                    );
                    assert!(
                        !last.map_or(false, is_no_end),
                        "{strategy} @ {max_w}: line ends with no-end: {:?}",
                        &para[l.lo..l.hi]
                    );
                }
            }
        }
    }

    #[test]
    fn wrap_kinsoku_keeps_no_start_punct_off_line_start() {
        // "aaaa。bbbb" at a width that fits "aaaa" alone but not "aaaa。" —
        // without kinsoku the period would start line 2; kinsoku pulls it
        // back onto line 1 (overflowing) instead.
        let Some(engine) = engine() else { return };
        let st = style();
        let text = format!("aaaa\u{3002}bbbb"); // aaaa。bbbb
        let px = |r: std::ops::Range<usize>| (r.end - r.start) as f32 * 10.0;
        let lines = wrap_optimal_with(&engine, &text, &st, 50.0, px);
        for l in &lines {
            let first = text[l.lo..].chars().next();
            assert!(
                !first.map_or(false, is_no_start),
                "line starts with no-start: {:?}",
                &text[l.lo..l.hi]
            );
        }
        // The period stayed with "aaaa" rather than starting its own line.
        assert!(
            text[lines[0].lo..lines[0].hi].contains('\u{3002}'),
            "kinsoku should keep the period on line 1: {:?}",
            lines.iter().map(|l| &text[l.lo..l.hi]).collect::<Vec<_>>()
        );
    }

    #[test]
    fn wrap_kinsoku_keeps_no_end_bracket_off_line_end() {
        // "aaaa（bbbb" — （ is a no-end opening bracket. At a width that
        // would otherwise leave （ alone at a line end, kinsoku pushes it
        // onto the next line with "bbbb".
        let Some(engine) = engine() else { return };
        let st = style();
        let text = format!("aaaa\u{ff08}bbbb"); // aaaa（bbbb
        let px = |r: std::ops::Range<usize>| (r.end - r.start) as f32 * 10.0;
        let lines = wrap_optimal_with(&engine, &text, &st, 50.0, px);
        for l in &lines {
            let last = text[..l.hi].chars().last();
            assert!(
                !last.map_or(false, is_no_end),
                "line ends with no-end: {:?}",
                &text[l.lo..l.hi]
            );
        }
    }

    #[test]
    fn shaper_compresses_fullwidth_brackets() {
        // Full-width CJK brackets carry ink in only one half of their em-box;
        // the shaper trims the empty ½-em half when a neighbour is present.
        // （你）should measure ≈ 2em (each bracket contributes ½em, 你 one em)
        // and （）alone ≈ 1em (two half-width brackets collapsed). If the
        // shaper were NOT compressing, these would be 3em and 2em.
        let Some(engine) = engine() else { return };
        let st = style();
        let one = engine.measure("\u{4f60}", &st).width; // 你 ≈ 1em
        assert!(one > 0.0);

        let w = engine.measure("\u{ff08}\u{4f60}\u{ff09}", &st).width; // （你）
        assert!(
            w < 2.8 * one && w > 1.6 * one,
            "（你）should compress to ~2em ({:.1}), got {w:.1}",
            2.0 * one
        );

        let ww = engine.measure("\u{ff08}\u{ff09}", &st).width; // （）
        assert!(
            (ww - one).abs() < 0.4 * one,
            "（）should collapse to ~1em ({:.1}), got {ww:.1}",
            one
        );
    }

    #[test]
    fn wrap_optimal_rebalances_a_stranded_word() {
        // Four atoms whose widths (via the callback) are 50, 50, 50, 30 px,
        // at a column that fits three 50s but not the 30 as well. Greedy
        // packs three onto line 1 and strands the 30 alone on line 2.
        // Knuth-Plass moves one 50 down so both lines are balanced — the
        // visible benefit of whole-paragraph optimisation over greedy fill.
        let Some(engine) = engine() else { return };
        let st = style();
        let text = "aaaaa aaaaa aaaaa aaa"; // byte widths 5,5,5,3
        let px = |r: std::ops::Range<usize>| (r.end - r.start) as f32 * 10.0; // -> 50,50,50,30
        let space_w = engine.measure(" ", &st).width;
        // Fits three 50s + two gaps; the fourth atom tips it over.
        let max_w = 3.0 * 50.0 + 2.0 * space_w + 10.0;

        let greedy = wrap_with(&engine, text, &st, max_w, px);
        let optimal = wrap_optimal_with(&engine, text, &st, max_w, px);

        // Both produce two lines; the difference is distribution.
        assert_eq!(greedy.len(), 2);
        assert_eq!(optimal.len(), 2);
        // Greedy's first line runs to the end of the third atom (byte 17);
        // optimal breaks earlier, moving one atom onto line 2.
        assert_eq!(greedy[0].hi, 17, "greedy fills line 1 with three atoms");
        assert!(
            optimal[0].hi < greedy[0].hi,
            "K-P should rebalance (break line 1 earlier), got hi={}",
            optimal[0].hi
        );
        // Contract still holds.
        assert_eq!(optimal[0].lo, 0);
        assert_eq!(optimal.last().unwrap().hi, text.len());
    }

    #[test]
    fn wrap_newline_is_hard_break() {
        // `\n` is a forced break: "a\nb" produces two one-atom lines, not
        // a single line that happens to fit. The previous wrap advertised
        // this in its docstring but actually treated `\n` like ordinary
        // whitespace; the rewrite honours the contract.
        let Some(engine) = engine() else { return };
        let st = style();
        let lines = wrap(&engine, "a\nb", &st, 10000.0);
        assert_eq!(lines.len(), 2, "newline must force a line break");
        assert_eq!(lines[0].lo, 0);
        assert_eq!(lines[0].hi, 1);
        assert_eq!(lines[1].lo, 2);
        assert_eq!(lines[1].hi, 3);
    }

    #[test]
    fn wrap_blank_line_between_newlines() {
        // `\n\n` should yield a blank visual line between the two atoms.
        let Some(engine) = engine() else { return };
        let st = style();
        let text = "a\n\nb";
        let lines = wrap(&engine, text, &st, 10000.0);
        // The middle line has no atoms — wrap drops it. Two atom lines is
        // the contract: atoms tile the input, whitespace only separates.
        assert_eq!(lines.len(), 2);
        assert_eq!(lines[0].lo, 0);
        assert_eq!(lines[0].hi, 1);
        assert_eq!(lines[1].lo, 3);
        assert_eq!(lines[1].hi, 4);
    }

    #[test]
    fn atom_is_cjk_reads_first_codepoint() {
        // Script class is derived from an atom's first codepoint, not
        // stored — verify the derivation at CJK/Latin boundaries.
        let s = "你好hello世界";
        let atoms = split_atoms(s);
        assert_eq!(atoms.len(), 5); // 你 好 hello 世 界
        assert!(atom_is_cjk(s, atoms[0])); // 你
        assert!(atom_is_cjk(s, atoms[1])); // 好
        assert!(!atom_is_cjk(s, atoms[2])); // hello
        assert!(atom_is_cjk(s, atoms[3])); // 世
        assert!(atom_is_cjk(s, atoms[4])); // 界
    }

    #[test]
    fn wrap_with_uses_caller_supplied_measure() {
        // A caller-supplied measure that reports each atom as 10px per
        // source byte — unrelated to the real glyph widths. wrap_with must
        // wrap against THAT width, proving atom widths route through
        // `measure` rather than the raw-source shape.
        let Some(engine) = engine() else { return };
        let st = style();
        let text = "alpha beta gamma delta epsilon";
        // Raw wrap at a wide column: one line.
        assert_eq!(wrap(&engine, text, &st, 10000.0).len(), 1);
        // Custom measure: 10px/byte. The string is 30 bytes => 300px, so a
        // 100px column must wrap to several lines.
        let measured = wrap_with(&engine, text, &st, 100.0, |r| {
            (r.end - r.start) as f32 * 10.0
        });
        assert!(
            measured.len() > 1,
            "wrap_with must honour the caller-supplied measure"
        );
        // The WrappedLine contract is unchanged: lines tile the input.
        assert_eq!(measured[0].lo, 0);
        for w in measured.windows(2) {
            assert!(w[0].hi <= w[1].lo);
        }
        assert_eq!(measured.last().unwrap().hi, text.len());
        // And the returned metric width is the caller's drawn width, not a
        // re-shape of the raw source.
        let first = &measured[0];
        let raw_width = engine.measure(&text[first.lo..first.hi], &st).width;
        assert!(
            (first.metrics.width - (first.hi - first.lo) as f32 * 10.0).abs() < 0.01,
            "metrics.width must come from `measure`, got {} vs raw {}",
            first.metrics.width,
            raw_width
        );
    }

    #[test]
    fn wrap_counts_interscript_autospace_gap() {
        // Two CJK<->Latin boundaries, no separators: the shaper inserts a
        // 1/4-em gap at each when this is drawn/measured. Wrap must count
        // the same gaps or a line it deems to "fit" would overflow on
        // screen. The contract: wrap's fit decision must agree with the
        // engine's measured width of the same string.
        let Some(engine) = engine() else { return };
        let st = style();
        let text = "你好hello你好";
        let drawn_w = engine.measure(text, &st).width;
        // At the drawn width (+slack) the whole string fits on one line.
        assert_eq!(wrap(&engine, text, &st, drawn_w + 1.0).len(), 1);
        // Shaving off exactly the two gaps must force a wrap — proving
        // wrap counted them. (If wrap ignored the gaps it would still
        // report one line here, under-counting vs the shaper.)
        let two_gaps = 2.0 * st.size_px * INTERSCRIPT_GAP_EM;
        assert!(
            wrap(&engine, text, &st, drawn_w - two_gaps - 0.5).len() > 1,
            "wrap must account for the inter-script gaps"
        );
    }

    #[test]
    fn wrap_cjk_each_char_is_break_opportunity() {
        // No whitespace, but CJK atoms wrap individually.
        let Some(engine) = engine() else { return };
        let st = style();
        let text = "你好世界你好世界";
        let wide = wrap(&engine, text, &st, 10000.0);
        assert_eq!(wide.len(), 1, "wide column keeps CJK on one line");
        // 8 CJK ideographs at 17 px each is ~136 px; force many breaks.
        let narrow = wrap(&engine, text, &st, 40.0);
        assert!(
            narrow.len() > 1,
            "narrow column must wrap CJK per-char, got {} lines",
            narrow.len()
        );
        // Atoms tile the input contiguously (no whitespace here).
        assert_eq!(narrow[0].lo, 0);
        for w in narrow.windows(2) {
            assert_eq!(w[0].hi, w[1].lo, "no gap between CJK lines");
        }
        assert_eq!(narrow.last().unwrap().hi, text.len());
    }

    #[test]
    fn wrap_long_input_is_linear() {
        // Smoke-test the O(n) wrap on a large input. With the old prefix-
        // remeasure implementation this scale of input was visibly slow;
        // the rewrite measures each atom once. The test only asserts
        // correctness of tiling, not timing.
        let Some(engine) = engine() else { return };
        let st = style();
        let words: Vec<String> = (0..2000).map(|i| format!("word{i}")).collect();
        let text = words.join(" ");
        let lines = wrap(&engine, &text, &st, 120.0);
        assert!(lines.len() > 1);
        assert_eq!(lines[0].lo, 0);
        for w in lines.windows(2) {
            assert!(w[0].hi <= w[1].lo);
            let gap = &text[w[0].hi..w[1].lo];
            assert!(
                gap.bytes().all(|b| b == b' ' || b == b'\n' || b == b'\t'),
                "gap between lines must be whitespace"
            );
        }
        assert_eq!(lines.last().unwrap().hi, text.len());
    }
}
