/*
 * scene_phong_skin.vert.slang — Phong skinning vertex shader in Slang.
 */

#include "scene_color.slang"

struct PushConstants {
    float4x4     mvp;
    PhongParams* params;
};

[[vk::push_constant]]
PushConstants pc;

struct VertexInput {
    [[vk::location(0)]] float3 a_position : POSITION0;
    [[vk::location(1)]] float3 a_normal   : NORMAL0;
    [[vk::location(2)]] float2 a_uv       : TEXCOORD0;
    [[vk::location(3)]] uint4  a_joints   : BLENDINDICES0;
    [[vk::location(4)]] float4 a_weights  : BLENDWEIGHT0;
};

struct VertexOutput {
    float4 sv_position : SV_Position;
    [[vk::location(0)]] float3 v_world_pos : POSITION0;
    [[vk::location(1)]] float3 v_world_nrm : NORMAL0;
    [[vk::location(2)]] float2 v_uv        : TEXCOORD0;
};

[shader("vertex")]
VertexOutput main(VertexInput input)
{
    VertexOutput output;
    float4 weights = max(input.a_weights, float4(0.0f, 0.0f, 0.0f, 0.0f));
    float total = dot(weights, float4(1.0f, 1.0f, 1.0f, 1.0f));
    weights = total > 0.0f ? weights / total : float4(1.0f, 0.0f, 0.0f, 0.0f);
    uint4 joints = min(input.a_joints, uint4(max(pc.params.joint_count, 1u) - 1u));

    float4x4 skin = weights.x * pc.params.palette[joints.x]
                  + weights.y * pc.params.palette[joints.y]
                  + weights.z * pc.params.palette[joints.z]
                  + weights.w * pc.params.palette[joints.w];

    float4 skinned_position = mul(skin, float4(input.a_position, 1.0f));
    float3x3 skin3 = float3x3(skin[0].xyz, skin[1].xyz, skin[2].xyz);
    float3 skinned_normal = mul(skin3, input.a_normal);

    output.sv_position = mul(pc.mvp, skinned_position);
    output.v_world_pos = mul(pc.params.world, skinned_position).xyz;
    float3x3 nrm = float3x3(pc.params.nrm0.xyz, pc.params.nrm1.xyz, pc.params.nrm2.xyz);
    output.v_world_nrm = mul(nrm, skinned_normal);
    output.v_uv = input.a_uv;
    return output;
}
