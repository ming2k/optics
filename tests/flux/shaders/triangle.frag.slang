/*
 * triangle.frag.slang — Hard-coded NDC triangle fragment shader in Slang.
 */

struct FragmentInput {
    [[vk::location(0)]] float3 v_color : COLOR0;
};

[shader("fragment")]
float4 main(FragmentInput input) : SV_Target {
    return float4(input.v_color, 1.0f);
}
