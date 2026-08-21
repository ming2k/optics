/*
 * Shared ICC profile synthesis for tests (ADR-0070). Static-inline
 * big-endian writer plus builders for the matrix+TRC and mft1 profile
 * flavours the parser tests exercise. No fixture files.
 */
#ifndef FLUX_TEST_ICC_KIT_H
#define FLUX_TEST_ICC_KIT_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct icc_writer {
    uint8_t buf[16384];
    size_t len;
    uint32_t tags[8 * 3]; /* sig, offset, size per tag */
    uint32_t tag_count;
} icc_writer;

static inline void w_u8(icc_writer *w, uint8_t v) {
    w->buf[w->len++] = v;
}
static inline void w_u16(icc_writer *w, uint16_t v) {
    w->buf[w->len++] = (uint8_t)(v >> 8);
    w->buf[w->len++] = (uint8_t)v;
}
static inline void w_u32(icc_writer *w, uint32_t v) {
    w->buf[w->len++] = (uint8_t)(v >> 24);
    w->buf[w->len++] = (uint8_t)(v >> 16);
    w->buf[w->len++] = (uint8_t)(v >> 8);
    w->buf[w->len++] = (uint8_t)v;
}
static inline void w_s15f16(icc_writer *w, double v) {
    w_u32(w, (uint32_t)(int32_t)lrint(v * 65536.0));
}
static inline void w_tag_sig(icc_writer *w, const char sig[4]) {
    w_u32(w, ((uint32_t)sig[0] << 24) | ((uint32_t)sig[1] << 16) | ((uint32_t)sig[2] << 8) |
                 (uint32_t)sig[3]);
}
static inline void w_align(icc_writer *w) {
    while (w->len & 3)
        w_u8(w, 0);
}

static inline void w_header_full(icc_writer *w, const char class_[4], const char pcs[4]) {
    w->len = 0;
    w->tag_count = 0;
    w_u32(w, 0); /* size, patched by icc_build_profile */
    w_u32(w, 0);
    w_u32(w, 0x04300000u);
    w_tag_sig(w, class_);
    w_tag_sig(w, "RGB ");
    w_tag_sig(w, pcs);
    w_u16(w, 2026);
    w_u16(w, 8);
    w_u16(w, 15);
    w_u16(w, 12);
    w_u16(w, 0);
    w_u16(w, 0);
    w_tag_sig(w, "acsp");
    w_u32(w, 0);
    w_u32(w, 0);
    w_u32(w, 0);
    w_u32(w, 0);
    w_u32(w, 0);
    w_u32(w, 0);
    w_u32(w, 0); /* intent: perceptual */
    w_s15f16(w, 0.9642);
    w_s15f16(w, 1.0);
    w_s15f16(w, 0.8251); /* D50 illuminant */
    w_u32(w, 0);
    w_u32(w, 0);
    w_u32(w, 0);
    w_u32(w, 0);
    w_u32(w, 0);
    for (int i = 0; i < 7; ++i)
        w_u32(w, 0);
}

static inline void w_tag_begin(icc_writer *w, const char sig[4]) {
    w_align(w);
    w->tags[w->tag_count * 3 + 0] = ((uint32_t)sig[0] << 24) | ((uint32_t)sig[1] << 16) |
                                    ((uint32_t)sig[2] << 8) | (uint32_t)sig[3];
    w->tags[w->tag_count * 3 + 1] = (uint32_t)w->len;
}
static inline void w_tag_end(icc_writer *w) {
    w_align(w);
    w->tags[w->tag_count * 3 + 2] = (uint32_t)w->len - w->tags[w->tag_count * 3 + 1];
    w->tag_count++;
}

static inline void w_xyz_tag(icc_writer *w, const char sig[4], double x, double y, double z) {
    w_tag_begin(w, sig);
    w_tag_sig(w, "XYZ ");
    w_u32(w, 0);
    w_s15f16(w, x);
    w_s15f16(w, y);
    w_s15f16(w, z);
    w_tag_end(w);
}

static inline void w_curv_gamma(icc_writer *w, const char sig[4], double gamma) {
    w_tag_begin(w, sig);
    w_tag_sig(w, "curv");
    w_u32(w, 0);
    w_u32(w, 1);
    w_u16(w, (uint16_t)lrint(gamma * 256.0));
    w_u16(w, 0);
    w_tag_end(w);
}

static inline void w_curv_table(icc_writer *w, const char sig[4], const uint16_t *table,
                                uint32_t n) {
    w_tag_begin(w, sig);
    w_tag_sig(w, "curv");
    w_u32(w, 0);
    w_u32(w, n);
    for (uint32_t i = 0; i < n; ++i)
        w_u16(w, table[i]);
    w_tag_end(w);
}

static inline void w_para_srgb(icc_writer *w, const char sig[4]) {
    w_tag_begin(w, sig);
    w_tag_sig(w, "para");
    w_u32(w, 0);
    w_u16(w, 4);
    w_u16(w, 0);
    w_s15f16(w, 2.4);
    w_s15f16(w, 1.0 / 1.055);
    w_s15f16(w, 0.055 / 1.055);
    w_s15f16(w, 1.0 / 12.92);
    w_s15f16(w, 0.04045);
    w_s15f16(w, 0.0);
    w_s15f16(w, 0.0);
    w_tag_end(w);
}

typedef struct icc_tag_def {
    uint32_t sig;
    size_t offset; /* into the data block */
    size_t size;
} icc_tag_def;

/* 'chad' chromatic adaptation matrix: 'sf32' + 9 row-major s15Fixed16. */
static inline void w_chad_tag(icc_writer *w, const char sig[4], const double m[9]) {
    w_tag_begin(w, sig);
    w_tag_sig(w, "sf32");
    w_u32(w, 0);
    for (int i = 0; i < 9; ++i)
        w_s15f16(w, m[i]);
    w_tag_end(w);
}

/* Assemble header + tag table + data block; returns the profile size. */
static inline size_t icc_build_profile(uint8_t *out, const char class_[4], const char pcs[4],
                                       const uint8_t *data, size_t data_size,
                                       const icc_tag_def *defs, uint32_t tag_count) {
    icc_writer h = {0};
    w_header_full(&h, class_, pcs);
    w_u32(&h, tag_count);
    size_t base = h.len + (size_t)tag_count * 12;
    for (uint32_t i = 0; i < tag_count; ++i) {
        w_u32(&h, defs[i].sig);
        w_u32(&h, (uint32_t)(base + defs[i].offset));
        w_u32(&h, (uint32_t)defs[i].size);
    }
    size_t total = base + data_size;
    h.buf[0] = (uint8_t)(total >> 24);
    h.buf[1] = (uint8_t)(total >> 16);
    h.buf[2] = (uint8_t)(total >> 8);
    h.buf[3] = (uint8_t)total;
    memcpy(out, h.buf, h.len);
    memcpy(out + h.len, data, data_size);
    return total;
}

/* sRGB colorants adapted to the D50 PCS (the published sRGB ICC values). */
static const double ICC_SRGB_D50[9] = {
    0.4360747, 0.2225045, 0.0139322, /* rXYZ */
    0.3850649, 0.7168786, 0.0971045, /* gXYZ */
    0.1430804, 0.0606169, 0.7141733, /* bXYZ */
};

/* Matrix tags + TRC tags; trc_kind: 0 = sRGB para, 1 = gamma 2.2 curv,
 * 2 = identity table (forces the parser's bake path), 3 = degenerate
 * parametric type 1 with a == 0 (hardening case). `chad`, when non-NULL,
 * appends a 'chad' chromatic adaptation tag (row-major doubles). */
static inline uint32_t icc_build_matrix_tags_ex(uint8_t *data, icc_tag_def *defs, int trc_kind,
                                                const double *chad, size_t *out_len) {
    icc_writer t = {0};
    w_xyz_tag(&t, "rXYZ", ICC_SRGB_D50[0], ICC_SRGB_D50[1], ICC_SRGB_D50[2]);
    w_xyz_tag(&t, "gXYZ", ICC_SRGB_D50[3], ICC_SRGB_D50[4], ICC_SRGB_D50[5]);
    w_xyz_tag(&t, "bXYZ", ICC_SRGB_D50[6], ICC_SRGB_D50[7], ICC_SRGB_D50[8]);
    static const char trc_sigs[3][4] = {"rTRC", "gTRC", "bTRC"};
    static const uint16_t ident[2] = {0, 65535};
    for (int i = 0; i < 3; ++i) {
        if (trc_kind == 0)
            w_para_srgb(&t, trc_sigs[i]);
        else if (trc_kind == 1)
            w_curv_gamma(&t, trc_sigs[i], 2.2);
        else if (trc_kind == 3) {
            /* parametricCurveType 1 with a == 0: the -b/a threshold is
             * degenerate; the parser must not produce inf/NaN. */
            w_tag_begin(&t, trc_sigs[i]);
            w_tag_sig(&t, "para");
            w_u32(&t, 0);
            w_u16(&t, 1);
            w_u16(&t, 0);
            w_s15f16(&t, 2.2);  /* g */
            w_s15f16(&t, 0.0);  /* a */
            w_s15f16(&t, 0.25); /* b */
            w_tag_end(&t);
        } else
            w_curv_table(&t, trc_sigs[i], ident, 2);
    }
    if (chad)
        w_chad_tag(&t, "chad", chad);
    memcpy(data, t.buf, t.len);
    for (uint32_t i = 0; i < t.tag_count; ++i) {
        defs[i].sig = t.tags[i * 3 + 0];
        defs[i].offset = t.tags[i * 3 + 1];
        defs[i].size = t.tags[i * 3 + 2];
    }
    *out_len = t.len;
    return t.tag_count;
}

static inline uint32_t icc_build_matrix_tags(uint8_t *data, icc_tag_def defs[6], int trc_kind,
                                             size_t *out_len) {
    return icc_build_matrix_tags_ex(data, defs, trc_kind, nullptr, out_len);
}

/* The published sRGB profile's 'chad': Bradford D65 -> D50. */
static const double ICC_CHAD_BRADFORD_D65_D50[9] = {
    0.9555766, -0.0230393, 0.0631636,  -0.0282895, 1.0099416,
    0.0210077, 0.0122982,  -0.0204830, 1.3299098,
};

/* Minimal lutAtoBType ('mAB '): header + three 2-entry identity 'curv'
 * tables as the B element (no matrix, M, CLUT, or A). Channel counts are
 * parameters so the parser's in/out validation is exercisable. */
static inline size_t icc_build_mab_identity(uint8_t *data, icc_tag_def *def, uint8_t in_ch,
                                            uint8_t out_ch) {
    icc_writer t = {0};
    w_tag_begin(&t, "A2B0");
    w_tag_sig(&t, "mAB ");
    w_u32(&t, 0);
    w_u8(&t, in_ch);
    w_u8(&t, out_ch);
    w_u8(&t, 0);
    w_u8(&t, 0);
    w_u32(&t, 32); /* B curves: right after the 32-byte header */
    w_u32(&t, 0);  /* matrix */
    w_u32(&t, 0);  /* M curves */
    w_u32(&t, 0);  /* CLUT */
    w_u32(&t, 0);  /* A curves */
    for (int ch = 0; ch < 3; ++ch) {
        w_tag_sig(&t, "curv");
        w_u32(&t, 0);
        w_u32(&t, 2);
        w_u16(&t, 0);
        w_u16(&t, 65535);
    }
    w_tag_end(&t);
    memcpy(data, t.buf, t.len);
    def->sig = t.tags[0];
    def->offset = t.tags[1];
    def->size = t.tags[2];
    return t.len;
}

/* mft2 A2B0 holding a constant Lab PCS colour: identity 2-entry input
 * and output tables, a 2^3 CLUT repeating one encoded Lab triple. `v2`
 * selects the ICC v2 16-bit Lab encoding (0xFF00 full scale) over the
 * v4 encoding (0xFFFF). Pair with pcs "Lab " and a matching profile
 * version (byte 8 < 4 for v2). */
static inline size_t icc_build_mft2_constant_lab(uint8_t *data, icc_tag_def *def, double L,
                                                 double a, double b, bool v2) {
    icc_writer t = {0};
    w_tag_begin(&t, "A2B0");
    w_tag_sig(&t, "mft2");
    w_u32(&t, 0);
    w_u8(&t, 3);
    w_u8(&t, 3);
    w_u8(&t, 2); /* grid */
    w_u8(&t, 0);
    for (int i = 0; i < 9; ++i)
        w_s15f16(&t, i % 4 == 0 ? 1.0 : 0.0); /* matrix (applies in mft2) */
    w_u16(&t, 2);                             /* input table entries */
    w_u16(&t, 2);                             /* output table entries */
    for (int ch = 0; ch < 3; ++ch) {
        w_u16(&t, 0);
        w_u16(&t, 65535); /* identity input tables */
    }
    const double full = v2 ? 65280.0 : 65535.0;
    const uint16_t enc[3] = {
        (uint16_t)lrint(L / 100.0 * full),
        (uint16_t)lrint((a + 128.0) / 255.0 * full),
        (uint16_t)lrint((b + 128.0) / 255.0 * full),
    };
    for (int i = 0; i < 8; ++i)
        for (int ch = 0; ch < 3; ++ch)
            w_u16(&t, enc[ch]);
    for (int ch = 0; ch < 3; ++ch) {
        w_u16(&t, 0);
        w_u16(&t, 65535); /* identity output tables */
    }
    w_tag_end(&t);
    memcpy(data, t.buf, t.len);
    def->sig = t.tags[0];
    def->offset = t.tags[1];
    def->size = t.tags[2];
    return t.len;
}

/* mft1 A2B0 with identity curves and a constant D50-white CLUT. */
static inline size_t icc_build_mft1_constant_white(uint8_t *data, icc_tag_def *def) {
    icc_writer t = {0};
    w_tag_begin(&t, "A2B0");
    w_tag_sig(&t, "mft1");
    w_u32(&t, 0);
    w_u8(&t, 3);
    w_u8(&t, 3);
    w_u8(&t, 2); /* grid */
    w_u8(&t, 0);
    for (int i = 0; i < 9; ++i)
        w_s15f16(&t, i % 4 == 0 ? 1.0 : 0.0); /* diagonal (mft1 ignores it) */
    for (int ch = 0; ch < 3; ++ch)
        for (int i = 0; i < 256; ++i)
            w_u8(&t, (uint8_t)i);
    for (int i = 0; i < 8 * 3; ++i) {
        static const double d50[3] = {0.9642, 1.0, 0.8251};
        w_u8(&t, (uint8_t)lrint(d50[i % 3] * 255.0));
    }
    for (int ch = 0; ch < 3; ++ch)
        for (int i = 0; i < 256; ++i)
            w_u8(&t, (uint8_t)i);
    w_tag_end(&t);
    memcpy(data, t.buf, t.len);
    def->sig = t.tags[0];
    def->offset = t.tags[1];
    def->size = t.tags[2];
    return t.len;
}

#endif /* FLUX_TEST_ICC_KIT_H */
