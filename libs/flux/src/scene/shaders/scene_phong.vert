#version 450

/*
 * Blinn-Phong scene vertex shader.
 *
 * Vertex format matches flux_vertex (position vec3, normal vec3,
 * uv vec2 — 32 bytes per vertex). uv is accepted for compatibility
 * with the mesh format but unused by phong.
 *
 * Lighting parameters ride a per-draw transient block referenced by
 * buffer device address; push constants carry only the MVP and the
 * 64-bit block address. Block layout matches scene_phong_params in
 * src/scene/scene.c.
 */

#extension GL_EXT_buffer_reference : enable

layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_uv;       /* unused for phong */

layout(buffer_reference, std430) readonly buffer PhongParams {
    mat4 world;
    vec4 nrm0;                 /* normal matrix columns:                */
    vec4 nrm1;                 /*   transpose(inverse(mat3(world)))     */
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

layout(location = 0) out vec3 v_world_pos;
layout(location = 1) out vec3 v_world_nrm;

void main()
{
    gl_Position = pc.mvp * vec4(a_position, 1.0);
    v_world_pos = (pc.params.world * vec4(a_position, 1.0)).xyz;

    mat3 nrm = mat3(pc.params.nrm0.xyz, pc.params.nrm1.xyz, pc.params.nrm2.xyz);
    v_world_nrm = nrm * a_normal;
}
