// Shadow-map depth pass (plan F3 Task 7): renders the casters of one shadow-atlas tile into the D32 atlas (no colour
// target). The vertex stage is the PBR one's position path (shaders/pbr/pbr.hlsl VSMain: morph deltas, then skinning,
// then the node's world matrix) with Frame.viewProj = the tile's light view-projection, so a skinned / morphed mesh
// casts the shadow of the pose it is drawn in. The pixel stage writes no colour; MASK materials discard below
// alphaCutoff exactly as pbr.hlsl (baseColorFactor x baseColorTexture x COLOR_0 alpha); OPAQUE never discards. BLEND
// materials cast no shadow (the renderer does not draw them here).
//
// Permutations (shaders/shadow/permutations.json): TIER x FG_SKIN x FG_MORPH; the same bindings as the PBR shader
// (shaders/common/bindings.hlsli: space0 Frame, space2 Material + base colour, space3 Object / joints / morph).
#include "../common/bindings.hlsli"
#include "../common/vertex.hlsli"
#include "../common/skin.hlsli"
#include "../common/morph.hlsli"

struct ShadowVSOutput
{
    float4 position : SV_Position;
    float2 uv0 : TEXCOORD0;
    float2 uv1 : TEXCOORD1;
    float4 color : COLOR0;
};

ShadowVSOutput VSMain(VSInput input)
{
    const MorphDelta morph = morphDelta(input);
    float4 position = float4(input.position + morph.position, 1.0);
#if FG_SKIN
    position = mul(skinMatrix(input, 0u), position);
#endif
    ShadowVSOutput o;
    o.position = mul(viewProj, mul(world, position));
    o.uv0 = input.uv0;
    o.uv1 = input.uv1;
    o.color = input.color;
    return o;
}

void PSMain(ShadowVSOutput input)
{
    // getBaseColor() alpha with TEXCOORD_n selection + KHR_texture_transform (pbr.hlsl textureUV).
    const float2 uv = texCoordSets.x == 1u ? input.uv1 : input.uv0;
    const float4 r0 = uvTransform[2u * TEX_BASE_COLOR];
    const float4 r1 = uvTransform[2u * TEX_BASE_COLOR + 1u];
    const float2 tuv = float2(dot(r0.xy, uv) + r0.z, dot(r1.xy, uv) + r1.z);
    const float alpha = baseColorFactor.a * baseColorTexture.SampleLevel(baseColorSampler, tuv, 0.0).a * input.color.a;
    if (materialFlags.x == 1u && alpha < alphaParams.x)
    {
        discard;
    }
}
