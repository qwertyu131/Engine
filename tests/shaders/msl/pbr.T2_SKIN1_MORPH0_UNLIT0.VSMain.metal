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
    uint out_var_DRAWFLAGS [[user(locn7)]];
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
    uint _82 = in.in_var_BLENDINDICES0.x * 2u;
    float4x4 _86 = jointMatrices._m0[_82] * in.in_var_BLENDWEIGHT0.x;
    uint _88 = in.in_var_BLENDINDICES0.y * 2u;
    float4x4 _92 = jointMatrices._m0[_88] * in.in_var_BLENDWEIGHT0.y;
    uint _106 = in.in_var_BLENDINDICES0.z * 2u;
    float4x4 _110 = jointMatrices._m0[_106] * in.in_var_BLENDWEIGHT0.z;
    uint _120 = in.in_var_BLENDINDICES0.w * 2u;
    float4x4 _124 = jointMatrices._m0[_120] * in.in_var_BLENDWEIGHT0.w;
    bool _135 = dot(in.in_var_BLENDWEIGHT0, float4(1.0)) == 0.0;
    float4x4 _138;
    if (_135)
    {
        _138 = float4x4(float4(1.0, 0.0, 0.0, 0.0), float4(0.0, 1.0, 0.0, 0.0), float4(0.0, 0.0, 1.0, 0.0), float4(0.0, 0.0, 0.0, 1.0));
    }
    else
    {
        _138 = float4x4(((float4(_86[0][0], _86[1][0], _86[2][0], _86[3][0]) + float4(_92[0][0], _92[1][0], _92[2][0], _92[3][0])) + float4(_110[0][0], _110[1][0], _110[2][0], _110[3][0])) + float4(_124[0][0], _124[1][0], _124[2][0], _124[3][0]), ((float4(_86[0][1], _86[1][1], _86[2][1], _86[3][1]) + float4(_92[0][1], _92[1][1], _92[2][1], _92[3][1])) + float4(_110[0][1], _110[1][1], _110[2][1], _110[3][1])) + float4(_124[0][1], _124[1][1], _124[2][1], _124[3][1]), ((float4(_86[0][2], _86[1][2], _86[2][2], _86[3][2]) + float4(_92[0][2], _92[1][2], _92[2][2], _92[3][2])) + float4(_110[0][2], _110[1][2], _110[2][2], _110[3][2])) + float4(_124[0][2], _124[1][2], _124[2][2], _124[3][2]), ((float4(_86[0][3], _86[1][3], _86[2][3], _86[3][3]) + float4(_92[0][3], _92[1][3], _92[2][3], _92[3][3])) + float4(_110[0][3], _110[1][3], _110[2][3], _110[3][3])) + float4(_124[0][3], _124[1][3], _124[2][3], _124[3][3]));
    }
    float4x4 _142 = jointMatrices._m0[_82 + 1u] * in.in_var_BLENDWEIGHT0.x;
    float4x4 _146 = jointMatrices._m0[_88 + 1u] * in.in_var_BLENDWEIGHT0.y;
    float4x4 _162 = jointMatrices._m0[_106 + 1u] * in.in_var_BLENDWEIGHT0.z;
    float4x4 _174 = jointMatrices._m0[_120 + 1u] * in.in_var_BLENDWEIGHT0.w;
    float4x4 _186;
    if (_135)
    {
        _186 = float4x4(float4(1.0, 0.0, 0.0, 0.0), float4(0.0, 1.0, 0.0, 0.0), float4(0.0, 0.0, 1.0, 0.0), float4(0.0, 0.0, 0.0, 1.0));
    }
    else
    {
        _186 = float4x4(((float4(_142[0][0], _142[1][0], _142[2][0], _142[3][0]) + float4(_146[0][0], _146[1][0], _146[2][0], _146[3][0])) + float4(_162[0][0], _162[1][0], _162[2][0], _162[3][0])) + float4(_174[0][0], _174[1][0], _174[2][0], _174[3][0]), ((float4(_142[0][1], _142[1][1], _142[2][1], _142[3][1]) + float4(_146[0][1], _146[1][1], _146[2][1], _146[3][1])) + float4(_162[0][1], _162[1][1], _162[2][1], _162[3][1])) + float4(_174[0][1], _174[1][1], _174[2][1], _174[3][1]), ((float4(_142[0][2], _142[1][2], _142[2][2], _142[3][2]) + float4(_146[0][2], _146[1][2], _146[2][2], _146[3][2])) + float4(_162[0][2], _162[1][2], _162[2][2], _162[3][2])) + float4(_174[0][2], _174[1][2], _174[2][2], _174[3][2]), ((float4(_142[0][3], _142[1][3], _142[2][3], _142[3][3]) + float4(_146[0][3], _146[1][3], _146[2][3], _146[3][3])) + float4(_162[0][3], _162[1][3], _162[2][3], _162[3][3])) + float4(_174[0][3], _174[1][3], _174[2][3], _174[3][3]));
    }
    float4 _208 = Object.world * (float4(in.in_var_POSITION, 1.0) * _138);
    float3 _224 = fast::normalize((Object.normalMatrix * float4(fast::normalize(in.in_var_NORMAL * float3x3(_186[0].xyz, _186[1].xyz, _186[2].xyz)), 0.0)).xyz);
    float3 _231 = fast::normalize((Object.world * float4(fast::normalize(in.in_var_TANGENT.xyz * float3x3(_138[0].xyz, _138[1].xyz, _138[2].xyz)), 0.0)).xyz);
    out.gl_Position = Frame.viewProj * _208;
    out.out_var_WORLD_POSITION = _208.xyz / float3(_208.w);
    out.out_var_TANGENT = _231;
    out.out_var_BINORMAL = cross(_224, _231) * in.in_var_TANGENT.w;
    out.out_var_NORMAL = _224;
    out.out_var_TEXCOORD0 = in.in_var_TEXCOORD0;
    out.out_var_TEXCOORD1 = in.in_var_TEXCOORD1;
    out.out_var_COLOR0 = in.in_var_COLOR0;
    out.out_var_DRAWFLAGS = Object.morphInfo.w;
    return out;
}

