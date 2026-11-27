/*
 * optics_gfx_sdf.slang — the single Slang definition of the analytic distance
 * fields (ADR-0118, ADR-0175) and of their derivatives (ADR-0148).
 *
 * <optics/gfx/sdf.h> is the host counterpart and evaluates the same expression;
 * tests/gfx/unit/test_sdf.c and test_sdf_parity pin the two differentially.
 */

#ifndef OPTICS_GFX_SDF_SLANG
#define OPTICS_GFX_SDF_SLANG

/* Border of half-width `hw` around a shape whose field is `d`: the ring is
 * the set |d| <= hw. `hw <= 0` returns `d` unchanged. */
float optics_gfx_sdf_ring(float d, float hw)
{
    return (hw > 0.0f) ? (abs(d) - hw) : d;
}

/* CSS Backgrounds & Borders Level 3 / Skia proportional radius normalization.
 * `radii` packs (tl, tr, br, bl) in clockwise order. */
float4 optics_gfx_corner_radii_normalize(float4 radii, float2 full_extent)
{
    if (!(full_extent.x > 0.0f) || !(full_extent.y > 0.0f))
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    float4 cr = max(radii, float4(0.0f, 0.0f, 0.0f, 0.0f));
    float max_r = max(max(cr.x, cr.y), max(cr.z, cr.w));
    float max_dim = max(full_extent.x, full_extent.y);
    if (max_r > 1e30f && max_r > max_dim)
        cr = (cr / max_r) * max_dim;

    float s = 1.0f;
    float top_sum = cr.x + cr.y;
    float bot_sum = cr.w + cr.z;
    float left_sum = cr.x + cr.w;
    float right_sum = cr.y + cr.z;
    if (top_sum > full_extent.x)
        s = min(s, full_extent.x / top_sum);
    if (bot_sum > full_extent.x)
        s = min(s, full_extent.x / bot_sum);
    if (left_sum > full_extent.y)
        s = min(s, full_extent.y / left_sum);
    if (right_sum > full_extent.y)
        s = min(s, full_extent.y / right_sum);
    return cr * s;
}

/* Corner arc distance and (optionally) gradient in the corner's local positive
 * quadrant (q.x > 0, q.y > 0). */
float optics_gfx_sdf_corner_arc(float2 q, float r, float c, bool with_normal, out float2 out_grad)
{
    float l2 = sqrt(dot(q, q));
    float d_l2 = l2 - r;
    float2 g_l2 = (with_normal && l2 > 1e-6f) ? (q / l2) : float2(0.0f, 0.0f);
    if (c <= 0.0f) {
        out_grad = g_l2;
        return d_l2;
    }

    float2 q2 = q * q;
    float q4 = sqrt(sqrt(q2.x * q2.x + q2.y * q2.y));
    if (q4 <= 1e-4f) {
        out_grad = g_l2;
        return d_l2;
    }

    float2 n = q / q4;
    float2 n2 = n * n;
    float2 n4 = n2 * n2;
    float g = sqrt(n4.x * n2.x + n4.y * n2.y);
    float d_l4 = (q4 - r) / g;

    if (with_normal) {
        float2 n3 = n2 * n;
        float inv_g = 1.0f / g;
        float k = (q4 - r) * inv_g * inv_g * 3.0f * inv_g / q4;
        float2 a = n3 * n2 * (1.0f - n4);
        float2 b = float2(n3.x * n4.y * n2.y, n3.y * n4.x * n2.x);
        float2 g_l4 = n3 * inv_g - k * (a - b);
        out_grad = g_l2 + (g_l4 - g_l2) * c;
    } else {
        out_grad = float2(0.0f, 0.0f);
    }
    return d_l2 + (d_l4 - d_l2) * c;
}

/* Single shared body for optics_gfx_sdf_rrect, _gradient, and _sample
 * (INV-SDF-02). `radii` packs (tl, tr, br, bl) in clockwise order. */
float optics_gfx_rrect_eval(float2 p, float2 half_extent, float4 radii, float curvature,
                            bool with_normal, bool crease_resolved, float aa, out float2 out_grad)
{
    float2 he = max(half_extent, float2(0.0f, 0.0f));
    float4 cr = optics_gfx_corner_radii_normalize(radii, 2.0f * he);
    float c = clamp(curvature, 0.0f, 1.0f);

    float2 b = abs(p) - he;
    float2 b_pos = max(b, float2(0.0f, 0.0f));
    float b_len = sqrt(dot(b_pos, b_pos));
    float d = b_len + min(max(b.x, b.y), 0.0f);

    float aa_eff = max(aa, 1e-4f);
    float2 g = float2(0.0f, 0.0f);
    if (with_normal) {
        if (b.x <= 0.0f && b.y <= 0.0f) {
            if (crease_resolved) {
                float h = clamp(0.5f + (b.x - b.y) / (2.0f * aa_eff), 0.0f, 1.0f);
                g = float2(sign(p.x) * h, sign(p.y) * (1.0f - h));
            } else {
                g = (b.x > b.y) ? float2(sign(p.x), 0.0f) : float2(0.0f, sign(p.y));
            }
        } else if (b_len > 1e-6f) {
            g = (b_pos / b_len) * sign(p);
        }
    }

    const float4 cr_arr = cr;
    const float corner_r[4] = {cr_arr.x, cr_arr.y, cr_arr.z, cr_arr.w};
    const float2 corner_s[4] = {
        float2(-1.0f, -1.0f),
        float2(1.0f, -1.0f),
        float2(1.0f, 1.0f),
        float2(-1.0f, 1.0f),
    };

    [unroll]
    for (int i = 0; i < 4; ++i) {
        float r = corner_r[i];
        if (r > 0.0f) {
            float2 s = corner_s[i];
            float2 q = s * p - (he - float2(r, r));
            if (q.x > 0.0f && q.y > 0.0f) {
                float2 ag;
                float da = optics_gfx_sdf_corner_arc(q, r, c, with_normal, ag);
                if (with_normal) {
                    float2 sg = ag * s;
                    if (crease_resolved && d < 0.0f && da < 0.0f && abs(da - d) < aa_eff) {
                        float h = clamp(0.5f + (da - d) / (2.0f * aa_eff), 0.0f, 1.0f);
                        g = g + (sg - g) * h;
                    } else if (da > d) {
                        g = sg;
                    }
                }
                if (da > d)
                    d = da;
            }
        }
    }

    out_grad = g;
    return d;
}

/* Exact analytic per-corner rounded rectangle / squircle SDF (ADR-0175).
 * `radii` packs (tl, tr, br, bl) in clockwise order. */
float optics_gfx_sdf_rrect(float2 p, float2 half_extent, float4 radii, float curvature)
{
    float2 unused;
    return optics_gfx_rrect_eval(p, half_extent, radii, curvature, false, false, 1.0f, unused);
}

/* Analytic unit gradient (continuum limit) of optics_gfx_sdf_rrect. */
float2 optics_gfx_sdf_rrect_gradient(float2 p, float2 half_extent, float4 radii, float curvature)
{
    float2 g;
    optics_gfx_rrect_eval(p, half_extent, radii, curvature, true, false, 1.0f, g);
    float len = length(g);
    return (len > 1e-12f) ? g / len : float2(0.0f, 1.0f);
}

/* Field and shading normal from ONE evaluation: returns float3(d, nx, ny)
 * with (nx, ny) unit vector. */
float3 optics_gfx_sdf_rrect_sample(float2 p, float2 half_extent, float4 radii, float curvature,
                                   float aa)
{
    float2 g;
    float d = optics_gfx_rrect_eval(p, half_extent, radii, curvature, true, true, aa, g);
    float len = length(g);
    return float3(d, (len > 1e-12f) ? g / len : float2(0.0f, 1.0f));
}

/* Polynomial smooth-minimum of two fields: the metaball union. */
float optics_gfx_smooth_union(float2 d, float radius, out float h)
{
    if (radius <= 0.001f) {
        h = (d.x < d.y) ? 1.0f : 0.0f;
        return min(d.x, d.y);
    }
    h = clamp(0.5f + 0.5f * (d.y - d.x) / radius, 0.0f, 1.0f);
    return lerp(d.y, d.x, h) - radius * h * (1.0f - h);
}

/* Gradient of optics_gfx_smooth_union with respect to p. */
float2 optics_gfx_smooth_union_gradient(float2 g_a, float2 g_b, float2 d, float radius)
{
    if (radius <= 0.001f)
        return (d.x < d.y) ? g_a : g_b;
    float h = clamp(0.5f + 0.5f * (d.y - d.x) / radius, 0.0f, 1.0f);
    float2 blended = lerp(g_b, g_a, h);
    float len = length(blended);
    return (len > 1e-12f) ? blended / len : g_a;
}

#endif /* OPTICS_GFX_SDF_SLANG */
