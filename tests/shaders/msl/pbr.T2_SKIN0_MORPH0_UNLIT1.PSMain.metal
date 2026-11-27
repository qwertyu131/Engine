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

struct type_Material
{
    float4 baseColorFactor;
    float4 emissiveFactor;
    float4 pbrParams;
    float4 alphaParams;
    uint4 materialFlags;
    uint4 texCoordSets;
    uint4 texCoordSets2;
    float4 uvTransform[10];
};

constant float4 _57 = {};

struct PSMain_out
{
    float4 out_var_SV_Target0 [[color(0)]];
};

struct PSMain_in
{
    float2 in_var_TEXCOORD0 [[user(locn4)]];
    float2 in_var_TEXCOORD1 [[user(locn5)]];
    float4 in_var_COLOR0 [[user(locn6)]];
};

fragment PSMain_out PSMain(PSMain_in in [[stage_in]], constant type_Frame& Frame [[buffer(0)]], constant type_Material& Material [[buffer(1)]], texture2d<float> baseColorTexture [[texture(0)]], sampler baseColorSampler [[sampler(0)]])
{
    PSMain_out out = {};
    float2 _71 = select(in.in_var_TEXCOORD0, in.in_var_TEXCOORD1, bool2(Material.texCoordSets.x == 1u));
    float4 _86 = baseColorTexture.sample(baseColorSampler, float2(dot(Material.uvTransform[0u].xy, _71) + Material.uvTransform[0u].z, dot(Material.uvTransform[1u].xy, _71) + Material.uvTransform[1u].z));
    float4 _88 = (Material.baseColorFactor * _86) * in.in_var_COLOR0;
    float4 _95;
    if (Material.materialFlags.x == 0u)
    {
        float4 _94 = _88;
        _94.w = 1.0;
        _95 = _94;
    }
    else
    {
        _95 = _88;
    }
    float4 _107;
    if (Material.materialFlags.x == 1u)
    {
        if (_95.w < Material.alphaParams.x)
        {
            discard_fragment();
        }
        float4 _106;
        _106.w = 1.0;
        _107 = _106;
    }
    else
    {
        _107 = _95;
    }
    float3 _112 = _95.xyz * Frame.iblParams.x;
    float3 _151;
    do
    {
        float _119 = precise::min(_112.x, precise::min(_112.y, _112.z));
        float _127;
        if (_119 < 0.07999999821186065673828125)
        {
            _127 = _119 - ((6.25 * _119) * _119);
        }
        else
        {
            _127 = 0.039999999105930328369140625;
        }
        float3 _129 = _112 - float3(_127);
        float _134 = precise::max(_129.x, precise::max(_129.y, _129.z));
        if (_134 < 0.7599999904632568359375)
        {
            _151 = _129;
            break;
        }
        float _140 = 1.0 - (0.0576000027358531951904296875 / (_134 + (-0.519999980926513671875)));
        _151 = mix(_129 * (_140 / _134), float3(1.0) * _140, float3(1.0 - (1.0 / ((0.1500000059604644775390625 * (_134 - _140)) + 1.0))));
        break;
    } while(false);
    float3 _156;
    if (Frame.frameFlags.x != 0u)
    {
        _156 = powr(_151, float3(0.4545454680919647216796875));
    }
    else
    {
        _156 = _151;
    }
    out.out_var_SV_Target0 = float4(_156, _107.w);
    return out;
}

