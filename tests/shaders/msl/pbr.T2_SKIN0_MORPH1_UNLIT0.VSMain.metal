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
};

vertex VSMain_out VSMain(VSMain_in in [[stage_in]], constant type_Frame& Frame [[buffer(0)]], constant type_Object& Object [[buffer(1)]], const device type_StructuredBuffer_v4float& morphDeltas [[buffer(2)]], const device type_StructuredBuffer_float& morphTargetWeights [[buffer(3)]], uint gl_VertexIndex [[vertex_id]])
{
    VSMain_out out = {};
    float3 _74;
    float3 _77;
    float3 _79;
    _74 = float3(0.0);
    _77 = float3(0.0);
    _79 = float3(0.0);
    for (uint _81 = 0u; _81 < Object.morphInfo.x; )
    {
        uint _91 = ((_81 * Object.morphInfo.y) + gl_VertexIndex) * 3u;
        _74 += (morphDeltas._m0[_91].xyz * morphTargetWeights._m0[_81]);
        _77 += (morphDeltas._m0[_91 + 1u].xyz * morphTargetWeights._m0[_81]);
        _79 += (morphDeltas._m0[_91 + 2u].xyz * morphTargetWeights._m0[_81]);
        _81++;
        continue;
    }
    float4 _120 = Object.world * float4(in.in_var_POSITION + _74, 1.0);
    float3 _136 = fast::normalize((Object.normalMatrix * float4(fast::normalize(in.in_var_NORMAL + _77), 0.0)).xyz);
    float3 _143 = fast::normalize((Object.world * float4(fast::normalize(in.in_var_TANGENT.xyz + _79), 0.0)).xyz);
    out.gl_Position = Frame.viewProj * _120;
    out.out_var_WORLD_POSITION = _120.xyz / float3(_120.w);
    out.out_var_TANGENT = _143;
    out.out_var_BINORMAL = cross(_136, _143) * in.in_var_TANGENT.w;
    out.out_var_NORMAL = _136;
    out.out_var_TEXCOORD0 = in.in_var_TEXCOORD0;
    out.out_var_TEXCOORD1 = in.in_var_TEXCOORD1;
    out.out_var_COLOR0 = in.in_var_COLOR0;
    return out;
}

