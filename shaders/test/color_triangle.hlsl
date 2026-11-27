// builtin:triangle shader (plan F2 Task 2): per-vertex colour, no resources, so the triangle draws on
// both RHI backends before bind groups exist (Task 3). shaders/test/triangle.hlsl (F1) needs a cbuffer
// and a texture; it stays the ShaderLibrary reflection fixture.

struct VSInput
{
    float2 position : POSITION; // NDC, +Y up (RHI convention)
    float4 color : COLOR0;
};

struct VSOutput
{
    float4 position : SV_Position;
    float4 color : COLOR0;
};

VSOutput VSMain(VSInput input)
{
    VSOutput o;
    o.position = float4(input.position, 0.0, 1.0);
    o.color = input.color;
    return o;
}

float4 PSMain(VSOutput input) : SV_Target0
{
    return input.color;
}
