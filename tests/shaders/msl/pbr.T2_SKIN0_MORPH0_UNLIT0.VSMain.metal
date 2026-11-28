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
};

vertex VSMain_out VSMain(VSMain_in in [[stage_in]], constant type_Frame& Frame [[buffer(0)]], constant type_Object& Object [[buffer(1)]])
{
    VSMain_out out = {};
    float4 _67 = Object.world * float4(in.in_var_POSITION, 1.0);
    float3 _83 = fast::normalize((Object.normalMatrix * float4(fast::normalize(in.in_var_NORMAL), 0.0)).xyz);
    float3 _90 = fast::normalize((Object.world * float4(fast::normalize(in.in_var_TANGENT.xyz), 0.0)).xyz);
    out.gl_Position = Frame.viewProj * _67;
    out.out_var_WORLD_POSITION = _67.xyz / float3(_67.w);
    out.out_var_TANGENT = _90;
    out.out_var_BINORMAL = cross(_83, _90) * in.in_var_TANGENT.w;
    out.out_var_NORMAL = _83;
    out.out_var_TEXCOORD0 = in.in_var_TEXCOORD0;
    out.out_var_TEXCOORD1 = in.in_var_TEXCOORD1;
    out.out_var_COLOR0 = in.in_var_COLOR0;
    out.out_var_DRAWFLAGS = Object.morphInfo.w;
    return out;
}

