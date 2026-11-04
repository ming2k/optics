/*
 * flux_sdf.glsl — the single GLSL definition of the analytic distance fields
 * (ADR-0118) and of their derivatives (ADR-0148).
 *
 * <flux/sdf.h> is the host counterpart and evaluates the same expression;
 * tests/flux/unit/test_sdf.c pins the two differentially. Include this file
 * rather than writing another copy: before it existed the rounded-rectangle
 * field was written out eight times (the canvas SDF shader, four Prism
 * materials, one Prism squircle variant, an inline copy in the canvas image
 * shader, and the software backend's host port), which is exactly the
 * duplicated-definition drift that ADR-0112's postmortem identifies.
 *
 * Requires GLSL 4.50 or later. No version directive: include it after yours.
 *
 * OpenGL conventions apply as everywhere else in this repository: `float`,
 * `vec2`, `length()`, `clamp()`, `mix()` are NOT shadowed or redefined here.
 *
 */

/* ------------------------------------------------------------------ */

/* PRECISION. This family runs 32-bit and only 32-bit, deliberately.
 *
 * It is safe at 16-bit — every intermediate below is bounded (`nx^4 + ny^4 == 1`
 * forces the L4 normalisation factor into [2^-1/4, 1], and each `sqrt(sqrt(x))`
 * pass sees a value <= 1), and a 16-bit build measures 0.05 degrees of normal
 * error. But it is not *worth* 16-bit: `q_pos` is pixel-magnitude, so `x^4`
 * overflows binary16 (max 65504) at a 24 px half extent unless the whole
 * decomposition is re-expressed in the normalised frame, and the field plus
 * gradient path is 337 of `shade_body`'s 708 instructions — a ceiling of roughly
 * 14% on a kernel that is otherwise texture fetch and bounded ALU. Paying for
 * two definitions of the field to buy that would be the duplication ADR-0118
 * exists to prevent. `test_sdf` asserts the bounded-intermediate property so a
 * future binary16 attempt starts from a proven precondition instead of
 * rediscovering this. See ADR-0148. */

/* ------------------------------------------------------------------ */

/* TWO EXPRESSIONS, ONE PINNED PAIR. The field-only entry points evaluate the
 * distance and nothing else; the derivative entry points evaluate the distance
 * and its gradient from shared intermediates. They are deliberately not one
 * function with a `want_gradient` flag: glslang does not eliminate gradient
 * arithmetic behind a run-time `if`, so a flag would make every coverage-only
 * consumer (the canvas SDF shader, frosted, acrylic, mica, backdrop frost) pay
 * ~40% more instructions for a gradient it discards. ADR-0118 defines
 * INV-SDF-02 as "one definition, any other evaluation a projection pinned by a
 * differential test", and test_sdf does exactly that: it asserts the derivative
 * path's `distance` equals the field path's distance bit for bit, so the two
 * cannot drift.
 */

/* The rounded-rectangle family is a corner decomposition: `q` is the signed
 * corner coordinate, `q_pos` its positive part, `inner` the interior term, and
 * a field in this family is "distance to the corner arc, plus the interior
 * term". The fields below expand that decomposition directly; the derivative
 * path decodes it into named locals. Keeping the two spellings apart is
 * deliberate: the four plain-rounded-rectangle materials (frosted, acrylic,
 * mica, backdrop frost) and the canvas SDF shader were already arithmetic-
 * minimal, and routing them through a shared struct measured ~5% more SPIR-V
 * in moves for values they never asked for. test_sdf pins every field against
 * its derivative path's distance, bit for bit, so the two spellings cannot
 * drift. */

/* ================================================================== */
/*  Fields — distance only                                            */
/* ================================================================== */

/* Signed distance to a rounded rectangle centred on the origin.
 *   half_extent: (hx, hy); negative components are clamped to zero
 *   radius:      clamped to [0, min(hx, hy)]
 * Negative inside, positive outside, zero on the boundary; the magnitude is
 * the Euclidean distance. `radius == 0` is a plain box. */
float flux_sdf_round_rect(vec2 p, vec2 half_extent, float radius)
{
    vec2 he = max(half_extent, vec2(0.0));
    float r = clamp(radius, 0.0, min(he.x, he.y));
    vec2 q = abs(p) - (he - vec2(r));
    return sqrt(dot(max(q, vec2(0.0)), max(q, vec2(0.0)))) + min(max(q.x, q.y), 0.0) - r;
}

/* Border of half-width `hw` around a shape whose field is `d`: the ring is
 * the set |d| <= hw. `hw <= 0` returns `d` unchanged, so this is a verbatim
 * replacement for the `if (stroke_hw > 0) d = abs(d) - stroke_hw;` idiom.
 * (`hw` rather than `half`: `half` is a reserved word in GLSL.) */
float flux_sdf_ring(float d, float hw)
{
    return (hw > 0.0) ? (abs(d) - hw) : d;
}

/* Squircle / continuous-curvature field: a rounded rectangle whose corner arc
 * blends a circular arc (`curvature == 0`, where this is *exactly*
 * flux_sdf_round_rect) with an L4 superellipse corner (`curvature == 1`).
 * Same conventions as flux_sdf_round_rect; the host counterpart is
 * flux_sdf_squircle in <flux/sdf.h>.
 *
 * `curvature` is a compile-time constant at every shipped call site, so the L4
 * block is eliminated for the plain-rounded-rectangle materials. There is no
 * `pow()` here: every exponent is a small integer or 1/4, so it is a multiply
 * or a nested square root. */
float flux_sdf_squircle(vec2 p, vec2 half_extent, float radius, float curvature)
{
    float c = clamp(curvature, 0.0, 1.0);
    vec2 he = max(half_extent, vec2(0.0));
    float r = clamp(radius, 0.0, min(he.x, he.y));
    vec2 q = abs(p) - (he - vec2(r));
    vec2 q_pos = max(q, vec2(0.0));
    float inner = min(max(q.x, q.y), 0.0);
    float d = sqrt(dot(q_pos, q_pos)) + inner - r;
    if (c > 0.0) {
        vec2 q2 = q_pos * q_pos;
        float q4_4 = q2.x * q2.x + q2.y * q2.y;    /* x^4 + y^4 */
        float q4 = sqrt(sqrt(q4_4));               /* ^(1/4)   */
        vec2 n = q_pos / q4;                       /* L4 direction */
        vec2 n2 = n * n;
        vec2 n4 = n2 * n2;
        float g = sqrt(n4.x * n2.x + n4.y * n2.y);  /* |grad q4|, in [2^-1/4, 1] */
        d += ((q4 - r) / g + inner - d) * c;
    }
    return d;
}

/* ================================================================== */
/*  Derivatives                                                       */
/* ================================================================== */

/* The one place the field has no gradient is its interior, q <= 0 on both
 * axes, where it degenerates to max(q.x, q.y) - r: two planes meeting along a
 * diagonal. Downstream materials shade that region, so the shipped normal used
 * to be recovered by differencing four field evaluations over a 1.5 px span.
 * That is four times the field work, first-order accurate, and — the part that
 * matters — it makes the normal a *neighbour* of the field rather than a
 * property of it.
 *
 * flux_sdf_squircle_gradient is the continuum answer: a hard pick of the
 * dominant edge, which is the correct limit and the wrong thing to shade with,
 * because it flips by 90 degrees across zero width. flux_sdf_squircle_sample is
 * the shading answer: the same analytic gradient everywhere it exists and, in
 * the interior only, a linear ramp of the two edge directions across one
 * sampling footprint `aa`. The ramp is what a central difference of that span
 * measures, so a consumer switching from taps to this call keeps its look and
 * loses three quarters of the field work. */

/* Gradient of the L4 corner arc with respect to q_pos, plus the field it came
 * from, from shared intermediates. `out_d` receives (q4 - r)/g; the caller adds
 * `inner`.
 *
 * q4 is 1-homogeneous, so its gradient is 0-homogeneous: it depends on the
 * direction n alone. Then g = |grad q4| = sqrt(nx^6 + ny^6) and, because
 * nx^4 + ny^4 == 1, g lies in [2^(-1/4), 1] — bounded by construction, no
 * clamp, and every intermediate is in [0, 1]. That bound is also what makes the
 * expression safe in half precision; the un-normalised form overflowed fp16 at
 * roughly 24 px of half extent. */
vec2 flux_l4_grad(vec2 q_pos, float r, out float out_d)
{
    vec2 q2 = q_pos * q_pos;
    float q4_4 = q2.x * q2.x + q2.y * q2.y;
    float q4 = sqrt(sqrt(q4_4));
    if (q4 <= 1e-4f) {
        out_d = -r; /* the q4 == 0 limit: d_l4 == inner - r, |grad q4| == 1 */
        return vec2(0.0);
    }
    vec2 n = q_pos / q4;
    vec2 n2 = n * n;
    vec2 n3 = n2 * n;
    vec2 n4 = n2 * n2;
    float g = sqrt(n4.x * n2.x + n4.y * n2.y);
    out_d = (q4 - r) / g;

    /* d q4/dp == n^3. With dn/dp = (I - n n^3)/q4, the chain rule through
     * sqrt(sum n^6) gives 3 n^5 (I - n n^3)/(g q4), and the quotient rule on
     * (q4 - r)/g collects into the two grouped terms below. Every term here is a
     * bounded ratio, which is why this stays exact in either width — but it runs
     * 32-bit regardless: `n^4` is already <= 1, so narrowing it buys nothing a
     * GPU notices, while mixing widths in a vector expression costs conversions. */
    float inv_g = 1.0f / g;
    float k = (q4 - r) * inv_g * inv_g * 3.0f * inv_g / q4;
    vec2 a = n3 * n2 * (1.0f - n4);
    vec2 b = vec2(n3.x * n4.y * n2.y, n3.y * n4.x * n2.x);
    return n3 * inv_g - k * (a - b);
}

/* Field and gradient of the active corner family, in one pass. `out_grad`
 * receives the outward gradient, unit-length at the call site's return. */
vec3 flux_squircle_field_grad(vec2 p, vec2 half_extent, float radius, float curvature,
                                bool crease_resolved, float aa, out vec2 out_grad)
{
    float c = clamp(curvature, 0.0, 1.0);
    vec2 he = max(half_extent, vec2(0.0));
    float r = clamp(radius, 0.0, min(he.x, he.y));
    vec2 q = abs(p) - (he - vec2(r));
    vec2 q_pos = max(q, vec2(0.0));
    float inner = min(max(q.x, q.y), 0.0);

    /* Distance and the L2 direction live at pixel magnitude: a `l2` of 464 px has
     * a binary16 ULP of 0.25 px, which is a quarter of a pixel of coverage error
     * on a large panel. 32-bit throughout — see the PRECISION note above. */
    float l2 = sqrt(dot(q_pos, q_pos));
    float d = l2 + inner - r;
    vec2 g = (l2 > 1e-6f) ? q_pos / l2 : vec2(0.0);

    if (c > 0.0) {
        float d_l4;
        vec2 g_l4 = flux_l4_grad(q_pos, r, d_l4);
        d += (d_l4 + inner - d) * c;
        g = g + (g_l4 - g) * c;
    }

    if (q.x <= 0.0f && q.y <= 0.0f) {
        /* Interior (both corner coordinates non-positive — exactly where the
         * field is a max() of two planes). `<=` and not `<`: at q.x == 0 with
         * q.y < 0 the exterior branch has q_pos == 0 and therefore no gradient
         * at all, so the boundary pixel belongs to this branch.
         *
         * A hard pick of the dominant edge is the continuum limit; the ramp is
         * what a central difference over one footprint reports, and is the only
         * supported shading behaviour. */
        if (crease_resolved) {
            float h = clamp(0.5f + (q.x - q.y) / (2.0f * max(aa, 1e-4f)), 0.0f, 1.0f);
            out_grad = vec2(sign(p.x) * h, sign(p.y) * (1.0f - h));
        } else {
            out_grad = (q.x > q.y) ? vec2(sign(p.x), 0.0) : vec2(0.0, sign(p.y));
        }
    } else {
        out_grad = g * sign(p);
    }
    return vec3(d, 0.0, 0.0);
}

vec2 flux_sdf_squircle_gradient(vec2 p, vec2 half_extent, float radius, float curvature)
{
    vec2 g;
    flux_squircle_field_grad(p, half_extent, radius, curvature, false, 1.0f, g);
    float len = length(g);
    return (len > 1e-12f) ? g / len : vec2(0.0, 1.0);
}

/* Field and shading normal from ONE evaluation: returns vec3(d, nx, ny) with
 * (nx, ny) a unit vector. `aa` is shape units per pixel; it is read only by the
 * interior ramp and is otherwise ignored, which is what "analytic" means — an
 * exact gradient does not know what a pixel is. */
vec3 flux_sdf_squircle_sample(vec2 p, vec2 half_extent, float radius, float curvature, float aa)
{
    vec2 g;
    vec3 d = flux_squircle_field_grad(p, half_extent, radius, curvature, true, aa, g);
    float len = length(g);
    return vec3(d.x, (len > 1e-12f) ? g / len : vec2(0.0, 1.0));
}

/* ================================================================== */
/*  Composition — a fused body is one field                        */
/* ================================================================== */

/* Polynomial smooth-minimum of two fields: the metaball union. `radius <= 0`
 * is a hard min. Returned alongside `h`, the blend weight, because the gradient
 * needs it and recomputing it would be a second definition of the blend. */
float flux_smooth_union(vec2 d, float radius, out float h)
{
    if (radius <= 0.001) {
        h = (d.x < d.y) ? 1.0 : 0.0;
        return min(d.x, d.y);
    }
    h = clamp(0.5 + 0.5 * (d.y - d.x) / radius, 0.0, 1.0);
    return mix(d.y, d.x, h) - radius * h * (1.0 - h);
}

/* Gradient of flux_smooth_union with respect to p, given both fields and their
 * unit gradients.
 *
 * The blend is `mix(d.y, d.x, h) - radius*h*(1-h)` with h a function of
 * position, so a full derivation carries a second term through dh/dp. That term
 * is identically zero: solving the blend's own `h` definition gives
 * `d.x - d.y == radius*(1 - 2h)` exactly, which is precisely the factor the
 * extra term would multiply. The gradient of a polynomial smooth-minimum is
 * therefore exactly linear in h — not an approximation, and the four
 * multiplications the "full" form would cost buy nothing. */
vec2 flux_smooth_union_gradient(vec2 g_a, vec2 g_b, vec2 d, float radius)
{
    if (radius <= 0.001) return (d.x < d.y) ? g_a : g_b;
    float h = clamp(0.5 + 0.5 * (d.y - d.x) / radius, 0.0, 1.0);
    vec2 blended = mix(g_b, g_a, h);
    float len = length(blended);
    return (len > 1e-12) ? blended / len : g_a;
}
