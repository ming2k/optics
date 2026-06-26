#version 450

/*
 * Blinn-Phong fragment shader: one directional light, half-vector
 * specular. Reads the per-draw parameter block via buffer device
 * address from the push constants (same block the vertex stage
 * reads; layout matches scene_phong_params in src/scene/scene.c).
 */

#extension GL_EXT_buffer_reference : enable

layout(buffer_reference, std430) readonly buffer PhongParams {
    mat4 world;
    vec4 nrm0;
    vec4 nrm1;
    vec4 nrm2;
    vec4 base_color;
    vec4 light_dir_shininess;  /* xyz = travel direction, w = exponent  */
    vec4 light_color_ambient;  /* rgb = light colour, w = ambient       */
    vec4 eye_specular;         /* xyz = world eye pos, w = strength     */
};

layout(push_constant) uniform PC {
    mat4        mvp;
    PhongParams params;
} pc;

layout(location = 0) in vec3 v_world_pos;
layout(location = 1) in vec3 v_world_nrm;

layout(location = 0) out vec4 out_color;

void main()
{
    vec3  N           = normalize(v_world_nrm);
    vec3  L           = normalize(-pc.params.light_dir_shininess.xyz);
    float shininess   = pc.params.light_dir_shininess.w;
    vec3  light_color = pc.params.light_color_ambient.rgb;
    float ambient     = pc.params.light_color_ambient.w;
    vec3  V           = normalize(pc.params.eye_specular.xyz - v_world_pos);
    float spec_str    = pc.params.eye_specular.w;

    float ndotl = max(dot(N, L), 0.0);

    vec3  H    = normalize(L + V);
    float spec = ndotl > 0.0 ? pow(max(dot(N, H), 0.0), shininess) : 0.0;

    vec3 base  = pc.params.base_color.rgb;
    vec3 color = base * ambient * light_color
               + base * ndotl   * light_color
               + spec_str * spec * light_color;

    out_color = vec4(color, pc.params.base_color.a);
}
