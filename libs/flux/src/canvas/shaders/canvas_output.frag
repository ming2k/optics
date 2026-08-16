#version 450
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require

/*
 * Canvas output-transform fragment shader (ADR-0069).
 *
 * One shader serves both directions of the per-pass color envelope:
 *
 *   encode (default): sample the working-space intermediate
 *     (premultiplied linear, scRGB), convert primaries working ->
 *     destination, apply the destination transfer function, dither
 *     when quantising. Runs as the final blit into the surface or a
 *     canvas render target.
 *
 *   decode (FLUX_OUTPUT_F_DECODE): sample an encoded destination
 *     (e.g. the previous swapchain contents on a LOAD pass), apply
 *     the inverse transfer and primaries destination -> working, and
 *     store working-space linear. Runs as the seed blit that primes
 *     the intermediate.
 *
 * Premultiplied values are converted on the straight colour and
 * re-premultiplied, matching src/math/color.c.
 *
 * Bindless layout:
 *   set 0, binding 0 — SAMPLED_IMAGE[]   (FLUX_BINDLESS_BIND_SAMPLED_IMAGE)
 *   set 0, binding 2 — SAMPLER[]         (FLUX_BINDLESS_BIND_SAMPLER)
 */

#include "canvas_color.glsl"

layout(set = 0, binding = 0) uniform texture2D u_textures[];
layout(set = 0, binding = 2) uniform sampler   u_samplers[];

#define FLUX_OUTPUT_F_DECODE   0x1u
#define FLUX_OUTPUT_F_NO_DITHER 0x2u

layout(push_constant) uniform PC {
    /* Destination <-> working primaries matrix, supplied for the
     * direction in use (encode: working -> destination; decode:
     * destination -> working). Columns are the CPU flux_mat3's columns,
     * one per push vec4. */
    vec4 primaries_rows[3];
    uint image_handle;
    uint sampler_handle;
    uint transfer;      /* flux_transfer_func of the ENCODED side */
    uint flags;
    float gamma;        /* FLUX_TF_GAMMA exponent */
    float dither_levels;/* 255 / 1023; ignored with NO_DITHER */
    float sdr_white_nits; /* Phase 3 (tone mapping); 203 default */
    float _pad;
} pc;

layout(location = 0) in  vec2 v_uv;
layout(location = 0) out vec4 out_color;

void main()
{
    uint ih = pc.image_handle   & 0x0FFFFFFFu;
    uint sh = pc.sampler_handle & 0x0FFFFFFFu;
    vec4 c = texture(
        sampler2D(
            u_textures[nonuniformEXT(ih)],
            u_samplers[nonuniformEXT(sh)]
        ), v_uv);

    mat3 primaries = mat3(pc.primaries_rows[0].xyz,
                          pc.primaries_rows[1].xyz,
                          pc.primaries_rows[2].xyz);

    float a = c.a;
    vec3 straight = a > 0.0 ? c.rgb / a : vec3(0.0);

    if ((pc.flags & FLUX_OUTPUT_F_DECODE) != 0u) {
        vec3 lin = flux_tf_decode3(int(pc.transfer), pc.gamma, straight);
        out_color = vec4(primaries * lin * a, a);
        return;
    }

    vec3 linear = primaries * straight;
    /* HDR destinations: rescale the scRGB working range (1.0 = 80 nits)
     * so SDR white lands on sdr_white_nits, then clamp to the signal
     * range. SDR destinations: roll off above-1.0 highlights. */
    if (pc.transfer == FLUX_TF_PQ) {
        linear = clamp(linear * (pc.sdr_white_nits / 10000.0), 0.0, 1.0);
    } else if (pc.transfer == FLUX_TF_HLG) {
        linear = clamp(linear * (pc.sdr_white_nits / 1000.0), 0.0, 1.0);
    } else if (pc.transfer != FLUX_TF_LINEAR) {
        linear = flux_tonemap_shoulder(linear);
    }
    vec3 encoded = flux_tf_encode3(int(pc.transfer), pc.gamma, linear);
    vec3 rgb = encoded * a;
    if ((pc.flags & FLUX_OUTPUT_F_NO_DITHER) == 0u)
        rgb += flux_dither_tpdf(gl_FragCoord.xy, pc.dither_levels);
    out_color = vec4(rgb, a);
}
