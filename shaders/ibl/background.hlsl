// Environment background of glTF scenes (plan F3 Task 5, r.Background=Environment): glTF-Sample-Renderer v1.1.0
// (d59d1d6) EnvironmentRenderer.drawEnvironmentMap (environment_renderer.js) + cubemap.vert / cubemap.frag:
//   the unit cube (8 corners, 36 indices as environment_renderer.js), drawn first with depth test off;
//   v_TexCoords = u_EnvRotation * a_position;
//   mat = u_ViewProjectionMatrix with column 3 replaced by (0, 0, 0, 0.1); gl_Position = (mat * a_position).xyww;
//   color = textureLod(u_GGXEnvSampler, v_TexCoords, u_EnvBlurNormalized * (u_MipCount - 1)) * u_EnvIntensity;
//   then toneMap (shaders/post/tonemap.hlsl).
// u_EnvBlurNormalized = 0.6 when blurEnvironmentMap (r.Background.Blur, reference mode 0.6), else 0.
// Binding convention (F1): space0 = frame; index unique per space.
#include "../post/tonemap.hlsl"

cbuffer Background : register(b0, space0)
{
    float4x4 viewProj;    // u_ViewProjectionMatrix (column 3 is replaced in the shader)
    float4x4 envRotation; // u_EnvRotation (upper 3x3)
    float4 params;        // x = u_Exposure, y = u_EnvIntensity, z = LOD = u_EnvBlurNormalized * (u_MipCount - 1)
    uint4 flags;          // x = 1: gamma 2.2 encode (UNORM target), y = tonemap operator
};
TextureCube<float4> environmentTexture : register(t1, space0); // u_GGXEnvSampler
SamplerState environmentSampler : register(s2, space0);

static const float3 kCorners[8] = {
    float3(-1, -1, -1), float3(1, -1, -1), float3(1, 1, -1), float3(-1, 1, -1),
    float3(-1, -1, 1),  float3(1, -1, 1),  float3(1, 1, 1),  float3(-1, 1, 1),
};
static const uint kIndices[36] = {
    1, 2, 0, 2, 3, 0, 6, 2, 1, 1, 5, 6, 6, 5, 4, 4, 7, 6, 6, 3, 2, 7, 3, 6, 3, 7, 0, 7, 4, 0, 5, 1, 0, 4, 5, 0,
};

struct VSOutput
{
    float4 position : SV_Position;
    float3 texCoords : TEXCOORD0;
};

VSOutput VSMain(uint vertexId : SV_VertexID)
{
    const float3 a_position = kCorners[kIndices[vertexId]];
    VSOutput o;
    o.texCoords = mul((float3x3)envRotation, a_position);
    float4x4 mat = viewProj;
    // GLSL mat[3] = vec4(0, 0, 0, 0.1) sets column 3; HLSL mat[r][c] is row r, column c.
    mat[0][3] = 0.0;
    mat[1][3] = 0.0;
    mat[2][3] = 0.0;
    mat[3][3] = 0.1;
    const float4 pos = mul(mat, float4(a_position, 1.0));
    o.position = pos.xyww;
    return o;
}

float4 PSMain(VSOutput input) : SV_Target0
{
    float4 color = environmentTexture.SampleLevel(environmentSampler, input.texCoords, params.z);
    color.rgb *= params.y;
    return float4(toneMapOutput(color.rgb, params.x, flags.x, flags.y), 1.0);
}
