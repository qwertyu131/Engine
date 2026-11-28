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

struct type_StructuredBuffer_v4float
{
    float4 _m0[1];
};

struct type_StructuredBuffer_float
{
    float _m0[1];
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

vertex VSMain_out VSMain(VSMain_in in [[stage_in]], constant type_Frame& Frame [[buffer(0)]], constant type_Object& Object [[buffer(1)]], const device type_StructuredBuffer_mat4v4float& jointMatrices [[buffer(2)]], const device type_StructuredBuffer_v4float& morphDeltas [[buffer(3)]], const device type_StructuredBuffer_float& morphTargetWeights [[buffer(4)]], uint gl_VertexIndex [[vertex_id]])
{
    VSMain_out out = {};
    float3 _93;
    float3 _96;
    float3 _98;
    _93 = float3(0.0);
    _96 = float3(0.0);
    _98 = float3(0.0);
    for (uint _100 = 0u; _100 < Object.morphInfo.x; )
    {
        uint _110 = ((_100 * Object.morphInfo.y) + gl_VertexIndex) * 3u;
        _93 += (morphDeltas._m0[_110].xyz * morphTargetWeights._m0[_100]);
        _96 += (morphDeltas._m0[_110 + 1u].xyz * morphTargetWeights._m0[_100]);
        _98 += (morphDeltas._m0[_110 + 2u].xyz * morphTargetWeights._m0[_100]);
        _100++;
        continue;
    }
    uint _136 = in.in_var_BLENDINDICES0.x * 2u;
    float4x4 _140 = jointMatrices._m0[_136] * in.in_var_BLENDWEIGHT0.x;
    uint _142 = in.in_var_BLENDINDICES0.y * 2u;
    float4x4 _146 = jointMatrices._m0[_142] * in.in_var_BLENDWEIGHT0.y;
    uint _160 = in.in_var_BLENDINDICES0.z * 2u;
    float4x4 _164 = jointMatrices._m0[_160] * in.in_var_BLENDWEIGHT0.z;
    uint _174 = in.in_var_BLENDINDICES0.w * 2u;
    float4x4 _178 = jointMatrices._m0[_174] * in.in_var_BLENDWEIGHT0.w;
    bool _189 = dot(in.in_var_BLENDWEIGHT0, float4(1.0)) == 0.0;
    float4x4 _192;
    if (_189)
    {
        _192 = float4x4(float4(1.0, 0.0, 0.0, 0.0), float4(0.0, 1.0, 0.0, 0.0), float4(0.0, 0.0, 1.0, 0.0), float4(0.0, 0.0, 0.0, 1.0));
    }
    else
    {
        _192 = float4x4(((float4(_140[0][0], _140[1][0], _140[2][0], _140[3][0]) + float4(_146[0][0], _146[1][0], _146[2][0], _146[3][0])) + float4(_164[0][0], _164[1][0], _164[2][0], _164[3][0])) + float4(_178[0][0], _178[1][0], _178[2][0], _178[3][0]), ((float4(_140[0][1], _140[1][1], _140[2][1], _140[3][1]) + float4(_146[0][1], _146[1][1], _146[2][1], _146[3][1])) + float4(_164[0][1], _164[1][1], _164[2][1], _164[3][1])) + float4(_178[0][1], _178[1][1], _178[2][1], _178[3][1]), ((float4(_140[0][2], _140[1][2], _140[2][2], _140[3][2]) + float4(_146[0][2], _146[1][2], _146[2][2], _146[3][2])) + float4(_164[0][2], _164[1][2], _164[2][2], _164[3][2])) + float4(_178[0][2], _178[1][2], _178[2][2], _178[3][2]), ((float4(_140[0][3], _140[1][3], _140[2][3], _140[3][3]) + float4(_146[0][3], _146[1][3], _146[2][3], _146[3][3])) + float4(_164[0][3], _164[1][3], _164[2][3], _164[3][3])) + float4(_178[0][3], _178[1][3], _178[2][3], _178[3][3]));
    }
    float4x4 _196 = jointMatrices._m0[_136 + 1u] * in.in_var_BLENDWEIGHT0.x;
    float4x4 _200 = jointMatrices._m0[_142 + 1u] * in.in_var_BLENDWEIGHT0.y;
    float4x4 _216 = jointMatrices._m0[_160 + 1u] * in.in_var_BLENDWEIGHT0.z;
    float4x4 _228 = jointMatrices._m0[_174 + 1u] * in.in_var_BLENDWEIGHT0.w;
    float4x4 _240;
    if (_189)
    {
        _240 = float4x4(float4(1.0, 0.0, 0.0, 0.0), float4(0.0, 1.0, 0.0, 0.0), float4(0.0, 0.0, 1.0, 0.0), float4(0.0, 0.0, 0.0, 1.0));
    }
    else
    {
        _240 = float4x4(((float4(_196[0][0], _196[1][0], _196[2][0], _196[3][0]) + float4(_200[0][0], _200[1][0], _200[2][0], _200[3][0])) + float4(_216[0][0], _216[1][0], _216[2][0], _216[3][0])) + float4(_228[0][0], _228[1][0], _228[2][0], _228[3][0]), ((float4(_196[0][1], _196[1][1], _196[2][1], _196[3][1]) + float4(_200[0][1], _200[1][1], _200[2][1], _200[3][1])) + float4(_216[0][1], _216[1][1], _216[2][1], _216[3][1])) + float4(_228[0][1], _228[1][1], _228[2][1], _228[3][1]), ((float4(_196[0][2], _196[1][2], _196[2][2], _196[3][2]) + float4(_200[0][2], _200[1][2], _200[2][2], _200[3][2])) + float4(_216[0][2], _216[1][2], _216[2][2], _216[3][2])) + float4(_228[0][2], _228[1][2], _228[2][2], _228[3][2]), ((float4(_196[0][3], _196[1][3], _196[2][3], _196[3][3]) + float4(_200[0][3], _200[1][3], _200[2][3], _200[3][3])) + float4(_216[0][3], _216[1][3], _216[2][3], _216[3][3])) + float4(_228[0][3], _228[1][3], _228[2][3], _228[3][3]));
    }
    float4 _262 = Object.world * (float4(in.in_var_POSITION + _93, 1.0) * _192);
    float3 _278 = fast::normalize((Object.normalMatrix * float4(fast::normalize((in.in_var_NORMAL + _96) * float3x3(_240[0].xyz, _240[1].xyz, _240[2].xyz)), 0.0)).xyz);
    float3 _285 = fast::normalize((Object.world * float4(fast::normalize((in.in_var_TANGENT.xyz + _98) * float3x3(_192[0].xyz, _192[1].xyz, _192[2].xyz)), 0.0)).xyz);
    out.gl_Position = Frame.viewProj * _262;
    out.out_var_WORLD_POSITION = _262.xyz / float3(_262.w);
    out.out_var_TANGENT = _285;
    out.out_var_BINORMAL = cross(_278, _285) * in.in_var_TANGENT.w;
    out.out_var_NORMAL = _278;
    out.out_var_TEXCOORD0 = in.in_var_TEXCOORD0;
    out.out_var_TEXCOORD1 = in.in_var_TEXCOORD1;
    out.out_var_COLOR0 = in.in_var_COLOR0;
    out.out_var_DRAWFLAGS = Object.morphInfo.w;
    return out;
}

