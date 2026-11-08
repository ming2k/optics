/* util.c — version accessors, the allocator seam, and the affine transform
 * algebra the parser and the painter both use.
 *
 * Libsvg is device-less by design (ADR-0136): a parsed document outlives every
 * flux device, so it cannot borrow a device allocator without making document
 * lifetime depend on device creation order. Allocation therefore goes to libc
 * and is released only by svg_release. */

#include "internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---- Version ---------------------------------------------------------- */

#define SVG_STR2(x) #x
#define SVG_STR(x) SVG_STR2(x)

void svg_version(int *major, int *minor, int *patch) {
    if (major)
        *major = SVG_VERSION_MAJOR;
    if (minor)
        *minor = SVG_VERSION_MINOR;
    if (patch)
        *patch = SVG_VERSION_PATCH;
}

uint32_t svg_version_number(void) {
    return SVG_VERSION_NUMBER;
}

bool svg_version_check(int major, int minor, int patch) {
    if (major != SVG_VERSION_MAJOR)
        return false;
    if (minor > SVG_VERSION_MINOR)
        return false;
    if (minor == SVG_VERSION_MINOR && patch > SVG_VERSION_PATCH)
        return false;
    return true;
}

const char *svg_version_string(void) {
    return SVG_STR(SVG_VERSION_MAJOR) "." SVG_STR(SVG_VERSION_MINOR) "." SVG_STR(SVG_VERSION_PATCH);
}

/* ---- Allocation ------------------------------------------------------- */

void *svg__alloc(size_t bytes) {
    if (bytes == 0)
        return nullptr;
    return malloc(bytes); // RAW_ALLOC_OK(no-device)
}

void *svg__realloc(void *ptr, size_t bytes) {
    if (bytes == 0) {
        free(ptr); // RAW_ALLOC_OK(no-device)
        return nullptr;
    }
    return realloc(ptr, bytes); // RAW_ALLOC_OK(no-device)
}

void svg__free(void *ptr) {
    free(ptr); // RAW_ALLOC_OK(no-device)
}

char *svg__strndup(const char *s, size_t n) {
    if (!s)
        return nullptr;
    char *out = (char *)svg__alloc(n + 1u);
    if (!out)
        return nullptr;
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

/* ---- Bounded substring search ------------------------------------------ */

const char *svg__find(const char *hay, size_t hay_len, const char *needle) {
    size_t n = strlen(needle);
    if (!hay || n == 0 || hay_len < n)
        return nullptr;
    /* memchr keeps the scan linear in practice and word-at-a-time, and its
     * bound is exact, so no byte at or past hay + hay_len is ever read. */
    size_t remaining = hay_len - n + 1;
    const char *p = hay;
    while (remaining > 0) {
        const char *hit = (const char *)memchr(p, needle[0], remaining);
        if (!hit)
            return nullptr;
        if (memcmp(hit, needle, n) == 0)
            return hit;
        size_t advanced = (size_t)(hit - p) + 1u;
        p += advanced;
        remaining -= advanced;
    }
    return nullptr;
}

/* ---- Growable path-command buffer ------------------------------------- */

bool svg_cmd_buf_push(svg_cmd_buf *b, svg_path_cmd value) {
    /* The ceiling is checked here rather than only in the parse loop: a single
     * `<polyline points="…">` can produce millions of commands from one element,
     * and the loop's after-the-fact check would let all of them be allocated
     * first. Refusing at the boundary keeps the limit a resource ceiling rather
     * than a memory-amplification vector. */
    if (b->limit && b->count >= b->limit)
        return false;
    if (b->count == b->cap) {
        uint32_t cap = b->cap ? b->cap * 2u : 64u;
        if (b->limit && cap > b->limit)
            cap = b->limit;
        svg_path_cmd *grown = (svg_path_cmd *)svg__realloc(b->data, (size_t)cap * sizeof *grown);
        if (!grown)
            return false;
        b->data = grown;
        b->cap = cap;
    }
    b->data[b->count++] = value;
    return true;
}

void svg_cmd_buf_deinit(svg_cmd_buf *b) {
    svg__free(b->data);
    *b = (svg_cmd_buf){};
}

/* ---- Affine transforms ------------------------------------------------ */

svg_transform svg__xform_identity(void) {
    return (svg_transform){1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
}

/* Result applies `rhs` first, then `lhs`. */
svg_transform svg__xform_mul(svg_transform lhs, svg_transform rhs) {
    return (svg_transform){
        .a = lhs.a * rhs.a + lhs.c * rhs.b,
        .b = lhs.b * rhs.a + lhs.d * rhs.b,
        .c = lhs.a * rhs.c + lhs.c * rhs.d,
        .d = lhs.b * rhs.c + lhs.d * rhs.d,
        .e = lhs.a * rhs.e + lhs.c * rhs.f + lhs.e,
        .f = lhs.b * rhs.e + lhs.d * rhs.f + lhs.f,
    };
}

svg_transform svg__xform_scale(float sx, float sy) {
    return (svg_transform){sx, 0.0f, 0.0f, sy, 0.0f, 0.0f};
}

svg_transform svg__xform_translate(float tx, float ty) {
    return (svg_transform){1.0f, 0.0f, 0.0f, 1.0f, tx, ty};
}

void svg__xform_point(svg_transform t, float x, float y, float *ox, float *oy) {
    *ox = t.a * x + t.c * y + t.e;
    *oy = t.b * x + t.d * y + t.f;
}

bool svg__xform_is_identity(svg_transform t) {
    return t.a == 1.0f && t.b == 0.0f && t.c == 0.0f && t.d == 1.0f && t.e == 0.0f && t.f == 0.0f;
}
