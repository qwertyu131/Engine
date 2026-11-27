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
    float3 _90;
    float3 _93;
    float3 _95;
    _90 = float3(0.0);
    _93 = float3(0.0);
    _95 = float3(0.0);
    for (uint _97 = 0u; _97 < Object.morphInfo.x; )
    {
        uint _107 = ((_97 * Object.morphInfo.y) + gl_VertexIndex) * 3u;
        _90 += (morphDeltas._m0[_107].xyz * morphTargetWeights._m0[_97]);
        _93 += (morphDeltas._m0[_107 + 1u].xyz * morphTargetWeights._m0[_97]);
        _95 += (morphDeltas._m0[_107 + 2u].xyz * morphTargetWeights._m0[_97]);
        _97++;
        continue;
    }
    uint _133 = in.in_var_BLENDINDICES0.x * 2u;
    float4x4 _137 = jointMatrices._m0[_133] * in.in_var_BLENDWEIGHT0.x;
    uint _139 = in.in_var_BLENDINDICES0.y * 2u;
    float4x4 _143 = jointMatrices._m0[_139] * in.in_var_BLENDWEIGHT0.y;
    uint _157 = in.in_var_BLENDINDICES0.z * 2u;
    float4x4 _161 = jointMatrices._m0[_157] * in.in_var_BLENDWEIGHT0.z;
    uint _171 = in.in_var_BLENDINDICES0.w * 2u;
    float4x4 _175 = jointMatrices._m0[_171] * in.in_var_BLENDWEIGHT0.w;
    bool _186 = dot(in.in_var_BLENDWEIGHT0, float4(1.0)) == 0.0;
    float4x4 _189;
    if (_186)
    {
        _189 = float4x4(float4(1.0, 0.0, 0.0, 0.0), float4(0.0, 1.0, 0.0, 0.0), float4(0.0, 0.0, 1.0, 0.0), float4(0.0, 0.0, 0.0, 1.0));
    }
    else
    {
        _189 = float4x4(((float4(_137[0][0], _137[1][0], _137[2][0], _137[3][0]) + float4(_143[0][0], _143[1][0], _143[2][0], _143[3][0])) + float4(_161[0][0], _161[1][0], _161[2][0], _161[3][0])) + float4(_175[0][0], _175[1][0], _175[2][0], _175[3][0]), ((float4(_137[0][1], _137[1][1], _137[2][1], _137[3][1]) + float4(_143[0][1], _143[1][1], _143[2][1], _143[3][1])) + float4(_161[0][1], _161[1][1], _161[2][1], _161[3][1])) + float4(_175[0][1], _175[1][1], _175[2][1], _175[3][1]), ((float4(_137[0][2], _137[1][2], _137[2][2], _137[3][2]) + float4(_143[0][2], _143[1][2], _143[2][2], _143[3][2])) + float4(_161[0][2], _161[1][2], _161[2][2], _161[3][2])) + float4(_175[0][2], _175[1][2], _175[2][2], _175[3][2]), ((float4(_137[0][3], _137[1][3], _137[2][3], _137[3][3]) + float4(_143[0][3], _143[1][3], _143[2][3], _143[3][3])) + float4(_161[0][3], _161[1][3], _161[2][3], _161[3][3])) + float4(_175[0][3], _175[1][3], _175[2][3], _175[3][3]));
    }
    float4x4 _193 = jointMatrices._m0[_133 + 1u] * in.in_var_BLENDWEIGHT0.x;
    float4x4 _197 = jointMatrices._m0[_139 + 1u] * in.in_var_BLENDWEIGHT0.y;
    float4x4 _213 = jointMatrices._m0[_157 + 1u] * in.in_var_BLENDWEIGHT0.z;
    float4x4 _225 = jointMatrices._m0[_171 + 1u] * in.in_var_BLENDWEIGHT0.w;
    float4x4 _237;
    if (_186)
    {
        _237 = float4x4(float4(1.0, 0.0, 0.0, 0.0), float4(0.0, 1.0, 0.0, 0.0), float4(0.0, 0.0, 1.0, 0.0), float4(0.0, 0.0, 0.0, 1.0));
    }
    else
    {
        _237 = float4x4(((float4(_193[0][0], _193[1][0], _193[2][0], _193[3][0]) + float4(_197[0][0], _197[1][0], _197[2][0], _197[3][0])) + float4(_213[0][0], _213[1][0], _213[2][0], _213[3][0])) + float4(_225[0][0], _225[1][0], _225[2][0], _225[3][0]), ((float4(_193[0][1], _193[1][1], _193[2][1], _193[3][1]) + float4(_197[0][1], _197[1][1], _197[2][1], _197[3][1])) + float4(_213[0][1], _213[1][1], _213[2][1], _213[3][1])) + float4(_225[0][1], _225[1][1], _225[2][1], _225[3][1]), ((float4(_193[0][2], _193[1][2], _193[2][2], _193[3][2]) + float4(_197[0][2], _197[1][2], _197[2][2], _197[3][2])) + float4(_213[0][2], _213[1][2], _213[2][2], _213[3][2])) + float4(_225[0][2], _225[1][2], _225[2][2], _225[3][2]), ((float4(_193[0][3], _193[1][3], _193[2][3], _193[3][3]) + float4(_197[0][3], _197[1][3], _197[2][3], _197[3][3])) + float4(_213[0][3], _213[1][3], _213[2][3], _213[3][3])) + float4(_225[0][3], _225[1][3], _225[2][3], _225[3][3]));
    }
    float4 _259 = Object.world * (float4(in.in_var_POSITION + _90, 1.0) * _189);
    float3 _275 = fast::normalize((Object.normalMatrix * float4(fast::normalize((in.in_var_NORMAL + _93) * float3x3(_237[0].xyz, _237[1].xyz, _237[2].xyz)), 0.0)).xyz);
    float3 _282 = fast::normalize((Object.world * float4(fast::normalize((in.in_var_TANGENT.xyz + _95) * float3x3(_189[0].xyz, _189[1].xyz, _189[2].xyz)), 0.0)).xyz);
    out.gl_Position = Frame.viewProj * _259;
    out.out_var_WORLD_POSITION = _259.xyz / float3(_259.w);
    out.out_var_TANGENT = _282;
    out.out_var_BINORMAL = cross(_275, _282) * in.in_var_TANGENT.w;
    out.out_var_NORMAL = _275;
    out.out_var_TEXCOORD0 = in.in_var_TEXCOORD0;
    out.out_var_TEXCOORD1 = in.in_var_TEXCOORD1;
    out.out_var_COLOR0 = in.in_var_COLOR0;
    return out;
}

