#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

struct type_Frame
{
    float4x4 viewProj;
    float4x4 envRotation;
    float4 cameraPosition;
    float4 iblParams;
    uint4 frameFlags;
};

struct type_Object
{
    float4x4 world;
    float4x4 normalMatrix;
    uint4 morphInfo;
    float4 morphWeights[2];
};

struct type_StructuredBuffer_mat4v4float
{
    float4x4 _m0[1];
};

struct VSMain_out
{
    float3 out_var_WORLD_POSITION [[user(locn0)]];
    float3 out_var_TANGENT [[user(locn1)]];
    float3 out_var_BINORMAL [[user(locn2)]];
    float3 out_var_NORMAL [[user(locn3)]];
    float2 out_var_TEXCOORD0 [[user(locn4)]];
    float2 out_var_TEXCOORD1 [[user(locn5)]];
    float4 out_var_COLOR0 [[user(locn6)]];
    float4 gl_Position [[position]];
};

struct VSMain_in
{
    float3 in_var_POSITION [[attribute(0)]];
    float3 in_var_NORMAL [[attribute(1)]];
    float4 in_var_TANGENT [[attribute(2)]];
    float2 in_var_TEXCOORD0 [[attribute(3)]];
    float2 in_var_TEXCOORD1 [[attribute(4)]];
    float4 in_var_COLOR0 [[attribute(5)]];
    uint4 in_var_BLENDINDICES0 [[attribute(6)]];
    float4 in_var_BLENDWEIGHT0 [[attribute(7)]];
};

vertex VSMain_out VSMain(VSMain_in in [[stage_in]], constant type_Frame& Frame [[buffer(0)]], constant type_Object& Object [[buffer(1)]], const device type_StructuredBuffer_mat4v4float& jointMatrices [[buffer(2)]])
{
    VSMain_out out = {};
    uint _77 = in.in_var_BLENDINDICES0.x * 2u;
    float4x4 _81 = jointMatrices._m0[_77] * in.in_var_BLENDWEIGHT0.x;
    uint _83 = in.in_var_BLENDINDICES0.y * 2u;
    float4x4 _87 = jointMatrices._m0[_83] * in.in_var_BLENDWEIGHT0.y;
    uint _101 = in.in_var_BLENDINDICES0.z * 2u;
    float4x4 _105 = jointMatrices._m0[_101] * in.in_var_BLENDWEIGHT0.z;
    uint _115 = in.in_var_BLENDINDICES0.w * 2u;
    float4x4 _119 = jointMatrices._m0[_115] * in.in_var_BLENDWEIGHT0.w;
    bool _130 = dot(in.in_var_BLENDWEIGHT0, float4(1.0)) == 0.0;
    float4x4 _133;
    if (_130)
    {
        _133 = float4x4(float4(1.0, 0.0, 0.0, 0.0), float4(0.0, 1.0, 0.0, 0.0), float4(0.0, 0.0, 1.0, 0.0), float4(0.0, 0.0, 0.0, 1.0));
    }
    else
    {
        _133 = float4x4(((float4(_81[0][0], _81[1][0], _81[2][0], _81[3][0]) + float4(_87[0][0], _87[1][0], _87[2][0], _87[3][0])) + float4(_105[0][0], _105[1][0], _105[2][0], _105[3][0])) + float4(_119[0][0], _119[1][0], _119[2][0], _119[3][0]), ((float4(_81[0][1], _81[1][1], _81[2][1], _81[3][1]) + float4(_87[0][1], _87[1][1], _87[2][1], _87[3][1])) + float4(_105[0][1], _105[1][1], _105[2][1], _105[3][1])) + float4(_119[0][1], _119[1][1], _119[2][1], _119[3][1]), ((float4(_81[0][2], _81[1][2], _81[2][2], _81[3][2]) + float4(_87[0][2], _87[1][2], _87[2][2], _87[3][2])) + float4(_105[0][2], _105[1][2], _105[2][2], _105[3][2])) + float4(_119[0][2], _119[1][2], _119[2][2], _119[3][2]), ((float4(_81[0][3], _81[1][3], _81[2][3], _81[3][3]) + float4(_87[0][3], _87[1][3], _87[2][3], _87[3][3])) + float4(_105[0][3], _105[1][3], _105[2][3], _105[3][3])) + float4(_119[0][3], _119[1][3], _119[2][3], _119[3][3]));
    }
    float4x4 _137 = jointMatrices._m0[_77 + 1u] * in.in_var_BLENDWEIGHT0.x;
    float4x4 _141 = jointMatrices._m0[_83 + 1u] * in.in_var_BLENDWEIGHT0.y;
    float4x4 _157 = jointMatrices._m0[_101 + 1u] * in.in_var_BLENDWEIGHT0.z;
    float4x4 _169 = jointMatrices._m0[_115 + 1u] * in.in_var_BLENDWEIGHT0.w;
    float4x4 _181;
    if (_130)
    {
        _181 = float4x4(float4(1.0, 0.0, 0.0, 0.0), float4(0.0, 1.0, 0.0, 0.0), float4(0.0, 0.0, 1.0, 0.0), float4(0.0, 0.0, 0.0, 1.0));
    }
    else
    {
        _181 = float4x4(((float4(_137[0][0], _137[1][0], _137[2][0], _137[3][0]) + float4(_141[0][0], _141[1][0], _141[2][0], _141[3][0])) + float4(_157[0][0], _157[1][0], _157[2][0], _157[3][0])) + float4(_169[0][0], _169[1][0], _169[2][0], _169[3][0]), ((float4(_137[0][1], _137[1][1], _137[2][1], _137[3][1]) + float4(_141[0][1], _141[1][1], _141[2][1], _141[3][1])) + float4(_157[0][1], _157[1][1], _157[2][1], _157[3][1])) + float4(_169[0][1], _169[1][1], _169[2][1], _169[3][1]), ((float4(_137[0][2], _137[1][2], _137[2][2], _137[3][2]) + float4(_141[0][2], _141[1][2], _141[2][2], _141[3][2])) + float4(_157[0][2], _157[1][2], _157[2][2], _157[3][2])) + float4(_169[0][2], _169[1][2], _169[2][2], _169[3][2]), ((float4(_137[0][3], _137[1][3], _137[2][3], _137[3][3]) + float4(_141[0][3], _141[1][3], _141[2][3], _141[3][3])) + float4(_157[0][3], _157[1][3], _157[2][3], _157[3][3])) + float4(_169[0][3], _169[1][3], _169[2][3], _169[3][3]));
    }
    float4 _203 = Object.world * (float4(in.in_var_POSITION, 1.0) * _133);
    float3 _219 = fast::normalize((Object.normalMatrix * float4(fast::normalize(in.in_var_NORMAL * float3x3(_181[0].xyz, _181[1].xyz, _181[2].xyz)), 0.0)).xyz);
    float3 _226 = fast::normalize((Object.world * float4(fast::normalize(in.in_var_TANGENT.xyz * float3x3(_133[0].xyz, _133[1].xyz, _133[2].xyz)), 0.0)).xyz);
    out.gl_Position = Frame.viewProj * _203;
    out.out_var_WORLD_POSITION = _203.xyz / float3(_203.w);
    out.out_var_TANGENT = _226;
    out.out_var_BINORMAL = cross(_219, _226) * in.in_var_TANGENT.w;
    out.out_var_NORMAL = _219;
    out.out_var_TEXCOORD0 = in.in_var_TEXCOORD0;
    out.out_var_TEXCOORD1 = in.in_var_TEXCOORD1;
    out.out_var_COLOR0 = in.in_var_COLOR0;
    return out;
}

