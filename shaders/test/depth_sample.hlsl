// Sampled depth (plan F3 Task 7: the shadow atlas is a D32 depth texture rendered as a depth attachment and read
// back as a texture by the lit pass). Used by tests/unit/rhi/test_depth_sample.cpp:
//   VSDepth / PSDepth  depth-only pass (no colour target): a fullscreen triangle at clip depth `depthValue.x`; the
//                      pixel stage writes no colour (void), it only discards where `depthValue.y` > 0 and x < 2;
//   VSMain / PSMain    colour pass: writes the depth texel under the pixel (Load, no sampler) to red.
cbuffer DepthParams : register(b0, space3)
{
    float4 depthValue; // x = clip-space depth of the depth-only triangle, y = 1: discard the first two columns
};
Texture2D<float> depthTexture : register(t0, space2);

struct VSOutput
{
    float4 position : SV_Position;
};

VSOutput fullscreen(uint id, float z)
{
    const float2 uv = float2(float((id << 1) & 2u), float(id & 2u));
    VSOutput o;
    o.position = float4(uv * 2.0 - 1.0, z, 1.0);
    return o;
}

VSOutput VSDepth(uint id : SV_VertexID)
{
    return fullscreen(id, depthValue.x);
}

void PSDepth(VSOutput input)
{
    if (depthValue.y > 0.0 && input.position.x < 2.0)
    {
        discard;
    }
}

VSOutput VSMain(uint id : SV_VertexID)
{
    return fullscreen(id, 0.0);
}

float4 PSMain(VSOutput input) : SV_Target0
{
    const float d = depthTexture.Load(int3(int2(input.position.xy), 0));
    return float4(d, 0.0, 0.0, 1.0);
}
