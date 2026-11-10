/*
 * flux_sdf.slang — the single Slang definition of the analytic distance fields
 * (ADR-0118) and of their derivatives (ADR-0148).
 *
 * <flux/sdf.h> is the host counterpart and evaluates the same expression;
 * tests/flux/unit/test_sdf.c and test_sdf_parity pin the two differentially.
 */

#ifndef FLUX_SDF_SLANG
#define FLUX_SDF_SLANG

/* Signed distance to a rounded rectangle centred on the origin. */
float flux_sdf_round_rect(float2 p, float2 half_extent, float radius)
{
    float2 he = max(half_extent, float2(0.0f, 0.0f));
    float r = clamp(radius, 0.0f, min(he.x, he.y));
    float2 q = abs(p) - (he - float2(r, r));
    float2 q_pos = max(q, float2(0.0f, 0.0f));
    return sqrt(dot(q_pos, q_pos)) + min(max(q.x, q.y), 0.0f) - r;
}

/* Border of half-width `hw` around a shape whose field is `d`: the ring is
 * the set |d| <= hw. `hw <= 0` returns `d` unchanged. */
float flux_sdf_ring(float d, float hw)
{
    return (hw > 0.0f) ? (abs(d) - hw) : d;
}

/* Squircle / continuous-curvature field: a rounded rectangle whose corner arc
 * blends a circular arc (`curvature == 0`) with an L4 superellipse corner (`curvature == 1`). */
float flux_sdf_squircle(float2 p, float2 half_extent, float radius, float curvature)
{
    float c = clamp(curvature, 0.0f, 1.0f);
    float2 he = max(half_extent, float2(0.0f, 0.0f));
    float r = clamp(radius, 0.0f, min(he.x, he.y));
    float2 q = abs(p) - (he - float2(r, r));
    float2 q_pos = max(q, float2(0.0f, 0.0f));
    float inner = min(max(q.x, q.y), 0.0f);
    float d = sqrt(dot(q_pos, q_pos)) + inner - r;
    if (c > 0.0f) {
        float2 q2 = q_pos * q_pos;
        float q4_4 = q2.x * q2.x + q2.y * q2.y;
        float q4 = sqrt(sqrt(q4_4));
        float2 n = q_pos / q4;
        float2 n2 = n * n;
        float2 n4 = n2 * n2;
        float g = sqrt(n4.x * n2.x + n4.y * n2.y);
        d += ((q4 - r) / g + inner - d) * c;
    }
    return d;
}

/* Gradient of the L4 corner arc with respect to q_pos, plus the field it came from. */
float2 flux_l4_grad(float2 q_pos, float r, out float out_d)
{
    float2 q2 = q_pos * q_pos;
    float q4_4 = q2.x * q2.x + q2.y * q2.y;
    float q4 = sqrt(sqrt(q4_4));
    if (q4 <= 1e-4f) {
        out_d = -r;
        return float2(0.0f, 0.0f);
    }
    float2 n = q_pos / q4;
    float2 n2 = n * n;
    float2 n3 = n2 * n;
    float2 n4 = n2 * n2;
    float g = sqrt(n4.x * n2.x + n4.y * n2.y);
    out_d = (q4 - r) / g;

    float inv_g = 1.0f / g;
    float k = (q4 - r) * inv_g * inv_g * 3.0f * inv_g / q4;
    float2 a = n3 * n2 * (1.0f - n4);
    float2 b = float2(n3.x * n4.y * n2.y, n3.y * n4.x * n2.x);
    return n3 * inv_g - k * (a - b);
}

/* Field and gradient of the active corner family, in one pass. */
float3 flux_squircle_field_grad(float2 p, float2 half_extent, float radius, float curvature,
                                bool crease_resolved, float aa, out float2 out_grad)
{
    float c = clamp(curvature, 0.0f, 1.0f);
    float2 he = max(half_extent, float2(0.0f, 0.0f));
    float r = clamp(radius, 0.0f, min(he.x, he.y));
    float2 q = abs(p) - (he - float2(r, r));
    float2 q_pos = max(q, float2(0.0f, 0.0f));
    float inner = min(max(q.x, q.y), 0.0f);

    float l2 = sqrt(dot(q_pos, q_pos));
    float d = l2 + inner - r;
    float2 g = (l2 > 1e-6f) ? q_pos / l2 : float2(0.0f, 0.0f);

    if (c > 0.0f) {
        float d_l4;
        float2 g_l4 = flux_l4_grad(q_pos, r, d_l4);
        d += (d_l4 + inner - d) * c;
        g = g + (g_l4 - g) * c;
    }

    if (q.x <= 0.0f && q.y <= 0.0f) {
        if (crease_resolved) {
            float h = clamp(0.5f + (q.x - q.y) / (2.0f * max(aa, 1e-4f)), 0.0f, 1.0f);
            out_grad = float2(sign(p.x) * h, sign(p.y) * (1.0f - h));
        } else {
            out_grad = (q.x > q.y) ? float2(sign(p.x), 0.0f) : float2(0.0f, sign(p.y));
        }
    } else {
        out_grad = g * sign(p);
    }
    return float3(d, 0.0f, 0.0f);
}

float2 flux_sdf_squircle_gradient(float2 p, float2 half_extent, float radius, float curvature)
{
    float2 g;
    flux_squircle_field_grad(p, half_extent, radius, curvature, false, 1.0f, g);
    float len = length(g);
    return (len > 1e-12f) ? g / len : float2(0.0f, 1.0f);
}

/* Field and shading normal from ONE evaluation: returns float3(d, nx, ny) with (nx, ny) unit vector. */
float3 flux_sdf_squircle_sample(float2 p, float2 half_extent, float radius, float curvature, float aa)
{
    float2 g;
    float3 d = flux_squircle_field_grad(p, half_extent, radius, curvature, true, aa, g);
    float len = length(g);
    return float3(d.x, (len > 1e-12f) ? g / len : float2(0.0f, 1.0f));
}

/* Polynomial smooth-minimum of two fields: the metaball union. */
float flux_smooth_union(float2 d, float radius, out float h)
{
    if (radius <= 0.001f) {
        h = (d.x < d.y) ? 1.0f : 0.0f;
        return min(d.x, d.y);
    }
    h = clamp(0.5f + 0.5f * (d.y - d.x) / radius, 0.0f, 1.0f);
    return lerp(d.y, d.x, h) - radius * h * (1.0f - h);
}

/* Gradient of flux_smooth_union with respect to p. */
float2 flux_smooth_union_gradient(float2 g_a, float2 g_b, float2 d, float radius)
{
    if (radius <= 0.001f) return (d.x < d.y) ? g_a : g_b;
    float h = clamp(0.5f + 0.5f * (d.y - d.x) / radius, 0.0f, 1.0f);
    float2 blended = lerp(g_b, g_a, h);
    float len = length(blended);
    return (len > 1e-12f) ? blended / len : g_a;
}

#endif /* FLUX_SDF_SLANG */
