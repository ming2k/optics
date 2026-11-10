/*
 * fill_pattern.comp.slang — Fills `count` floats with i * 0.01 in Slang.
 */

struct PushConstants {
    float*   out_buf;
    uint32_t count;
    uint32_t _pad;
};

[[vk::push_constant]]
PushConstants pc;

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint32_t i = dispatchThreadID.x;
    if (i >= pc.count) return;
    pc.out_buf[i] = float(i) * 0.01f;
}
