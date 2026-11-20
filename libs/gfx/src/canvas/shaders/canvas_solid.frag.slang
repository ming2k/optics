/*
 * canvas_solid.frag.slang — Solid-colour canvas fragment shader in Slang.
 */

struct FragmentInput {
    [[vk::location(0)]] float4 v_color : COLOR0;
    [[vk::location(1)]] float2 v_pos   : POSITION0;
};

[shader("fragment")]
float4 main(FragmentInput input) : SV_Target
{
    return input.v_color;
}
