// glTF vertex input and the VS -> FS interface of the PBR shader (plan F1 Task 4).
// One shared VSOutput struct for both stages: GLSL ES 3.00 links varyings by name and shaderc names
// them from their locations, so both stages must declare the same list in the same order.
//
// Vertex attributes (glTF -> HLSL semantic). The renderer supplies missing optional attributes
// (COLOR_0 = white, TEXCOORD_1 = 0, TANGENT = MikkTSpace-generated in F3).
//   POSITION, NORMAL, TANGENT (w = bitangent sign), TEXCOORD_0, TEXCOORD_1, COLOR_0
//   FG_SKIN:            JOINTS_0 -> BLENDINDICES0, WEIGHTS_0 -> BLENDWEIGHT0
//   FG_MORPH, TIER = 0: position deltas of the <= 8 active targets -> MORPH_POSITION0..7
// Worst case (T0, skin + morph) = 16 attributes = the GLES 3.0 / WebGL2 / D3D10 minimum.
#ifndef VERTEX_HLSLI
#define VERTEX_HLSLI

#include "bindings.hlsli"

struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float4 tangent : TANGENT;
    float2 uv0 : TEXCOORD0;
    float2 uv1 : TEXCOORD1;
    float4 color : COLOR0;
#if FG_SKIN
    uint4 joints : BLENDINDICES0;
    float4 weights : BLENDWEIGHT0;
#endif
#if FG_MORPH && TIER == 0
    float3 morphPosition0 : MORPH_POSITION0;
    float3 morphPosition1 : MORPH_POSITION1;
    float3 morphPosition2 : MORPH_POSITION2;
    float3 morphPosition3 : MORPH_POSITION3;
    float3 morphPosition4 : MORPH_POSITION4;
    float3 morphPosition5 : MORPH_POSITION5;
    float3 morphPosition6 : MORPH_POSITION6;
    float3 morphPosition7 : MORPH_POSITION7;
#endif
#if FG_MORPH && TIER >= 1
    uint vertexId : SV_VertexID;
#endif
};

struct VSOutput
{
    float4 position : SV_Position;
    float3 worldPosition : WORLD_POSITION; // Sample-Viewer v_Position
    float3 tangentW : TANGENT;             // v_TBN[0]
    float3 bitangentW : BINORMAL;          // v_TBN[1]
    float3 normalW : NORMAL;               // v_TBN[2]
    float2 uv0 : TEXCOORD0;
    float2 uv1 : TEXCOORD1;
    float4 color : COLOR0;
    // Object.morphInfo.w passed on by the vertex stage (bit 0 unlit points / lines, bit 1 no normal texture), so the
    // pixel stage reads no space-3 cbuffer and the space-3 layout stays the same in every permutation.
    nointerpolation uint drawFlags : DRAWFLAGS;
};

#endif // VERTEX_HLSLI
