/*
 * Scene color management (ADR-0069): the shared per-draw params blocks
 * and the base-color texel decode into the linear working space.
 *
 * The params blocks mirror scene_surface_params / scene_unlit_params /
 * scene_phong_params in src/scene/scene.c — keep the two in sync.
 */

#ifndef SCENE_COLOR_GLSL
#define SCENE_COLOR_GLSL

#include "canvas_color.glsl"

layout(set = 0, binding = 0) uniform texture2D u_textures[];
layout(set = 0, binding = 2) uniform sampler u_samplers[];

/* ADR-0069/0070: per-image color parameters (flux_image_color_params).
 * Same layout as canvas_image.frag; read only when
 * color_flags & SCENE_COLOR_HAS_PARAMS. */
struct ColorParams {
    vec4 primaries[3];  /* content -> working space, column-major mat3 */
    uint transfer;      /* flux_transfer_func */
    float gamma;
    uint lut_handle;    /* baked ICC 3D LUT (2D layout), 0xFFFFFFFF = none */
    uint lut_size;
};
layout(buffer_reference, std430) readonly buffer ColorParamsRef { ColorParams cp; };

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer JointPalette {
    mat4 joints[];
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer UnlitParams {
    vec4 base_color;
    vec4 uv_scale_offset;
    vec4 uv_rotation_alpha_cutoff;
    uvec4 texture_info;
    ColorParamsRef color_params;
    uint color_flags;
    JointPalette palette;
    uint joint_count;
};

layout(buffer_reference, std430) readonly buffer PhongParams {
    mat4 world;
    vec4 nrm0;
    vec4 nrm1;
    vec4 nrm2;
    vec4 base_color;
    vec4 uv_scale_offset;
    vec4 uv_rotation_alpha_cutoff;
    uvec4 texture_info;
    ColorParamsRef color_params;
    uint color_flags;
    vec4 light_dir_shininess;
    vec4 light_color_ambient;
    vec4 eye_specular;
    JointPalette palette;
    uint joint_count;
};

const uint SCENE_COLOR_DECODE     = 0x1u;
const uint SCENE_COLOR_HAS_PARAMS = 0x2u;

/* Baked ICC LUT: 2D-laid-out (N² × N), R fastest then G then B; the
 * blue slice is lerped manually. Same layout as canvas_image.frag. */
vec3 sample_lut3d(uint ih, uint sh, float n, vec3 e)
{
    vec3 c = clamp(e, 0.0, 1.0) * (n - 1.0);
    float b0 = floor(c.b);
    float b1 = min(b0 + 1.0, n - 1.0);
    vec2 uv0 = vec2((b0 * n + c.r + 0.5) / (n * n), (c.g + 0.5) / n);
    vec2 uv1 = vec2((b1 * n + c.r + 0.5) / (n * n), (c.g + 0.5) / n);
    vec3 s0 = texture(sampler2D(u_textures[nonuniformEXT(ih)],
                                u_samplers[nonuniformEXT(sh)]), uv0).rgb;
    vec3 s1 = texture(sampler2D(u_textures[nonuniformEXT(ih)],
                                u_samplers[nonuniformEXT(sh)]), uv1).rgb;
    return mix(s0, s1, c.b - b0);
}

/* Decode a tagged texel into the linear working space via its params
 * block (baked ICC LUT, or primaries matrix + transfer curve). Scene
 * content is straight-alpha (glTF), so the transform applies to rgb
 * directly — no premultiply round-trip. `sh` is the sampler handle,
 * reused for LUT reads.
 *
 * Dispatch stays at the call site — a readonly buffer reference cannot
 * be a formal parameter:
 *
 *   if ((flags & SCENE_COLOR_DECODE) != 0u)
 *       rgb = (flags & SCENE_COLOR_HAS_PARAMS) != 0u
 *                 ? scene_decode_tagged(params.color_params.cp, sh, rgb)
 *                 : flux_tf_decode3(FLUX_TF_SRGB, 0.0, rgb);
 */
vec3 scene_decode_tagged(ColorParams cp, uint sh, vec3 rgb)
{
    if (cp.lut_handle != 0xFFFFFFFFu)
        return sample_lut3d(cp.lut_handle, sh, float(cp.lut_size), rgb);
    mat3 prim = mat3(cp.primaries[0].xyz, cp.primaries[1].xyz, cp.primaries[2].xyz);
    return prim * flux_decode_to_working(int(cp.transfer), cp.gamma, rgb);
}

#endif /* SCENE_COLOR_GLSL */
