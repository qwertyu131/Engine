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

constant float4 _96 = {};

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
    float2 _110 = select(in.in_var_TEXCOORD0, in.in_var_TEXCOORD1, bool2(Material.texCoordSets.x == 1u));
    float4 _125 = baseColorTexture.sample(baseColorSampler, float2(dot(Material.uvTransform[0u].xy, _110) + Material.uvTransform[0u].z, dot(Material.uvTransform[1u].xy, _110) + Material.uvTransform[1u].z));
    float4 _127 = (Material.baseColorFactor * _125) * in.in_var_COLOR0;
    float4 _134;
    if (Material.materialFlags.x == 0u)
    {
        float4 _133 = _127;
        _133.w = 1.0;
        _134 = _133;
    }
    else
    {
        _134 = _127;
    }
    float4 _146;
    if (Material.materialFlags.x == 1u)
    {
        if (_134.w < Material.alphaParams.x)
        {
            discard_fragment();
        }
        float4 _145;
        _145.w = 1.0;
        _146 = _145;
    }
    else
    {
        _146 = _134;
    }
    float3 _153 = _134.xyz * Frame.iblParams.x;
    float3 _212;
    if (Frame.frameFlags.y == 0u)
    {
        float3 _196;
        do
        {
            float _164 = precise::min(_153.x, precise::min(_153.y, _153.z));
            float _172;
            if (_164 < 0.07999999821186065673828125)
            {
                _172 = _164 - ((6.25 * _164) * _164);
            }
            else
            {
                _172 = 0.039999999105930328369140625;
            }
            float3 _174 = _153 - float3(_172);
            float _179 = precise::max(_174.x, precise::max(_174.y, _174.z));
            if (_179 < 0.7599999904632568359375)
            {
                _196 = _174;
                break;
            }
            float _185 = 1.0 - (0.0576000027358531951904296875 / (_179 + (-0.519999980926513671875)));
            _196 = mix(_174 * (_185 / _179), float3(1.0) * _185, float3(1.0 - (1.0 / ((0.1500000059604644775390625 * (_179 - _185)) + 1.0))));
            break;
        } while(false);
        _212 = _196;
    }
    else
    {
        float3 _211;
        if (Frame.frameFlags.y == 1u)
        {
            float3 _200 = float3x3(float3(0.59719002246856689453125, 0.075999997556209564208984375, 0.0284000001847743988037109375), float3(0.354579985141754150390625, 0.908339977264404296875, 0.13382999598979949951171875), float3(0.048229999840259552001953125, 0.0156599991023540496826171875, 0.837769985198974609375)) * _153;
            _211 = fast::clamp(float3x3(float3(1.60475003719329833984375, -0.10208000242710113525390625, -0.00326999998651444911956787109375), float3(-0.5310800075531005859375, 1.108129978179931640625, -0.07276000082492828369140625), float3(-0.0736699998378753662109375, -0.00604999996721744537353515625, 1.0760200023651123046875)) * (((_200 * (_200 + float3(0.02457859925925731658935546875))) - float3(9.0537003416102379560470581054688e-05)) / ((_200 * ((_200 * 0.98372900485992431640625) + float3(0.4329510033130645751953125))) + float3(0.23808099329471588134765625))), float3(0.0), float3(1.0));
        }
        else
        {
            _211 = _153;
        }
        _212 = _211;
    }
    float3 _217;
    if (Frame.frameFlags.x != 0u)
    {
        _217 = powr(_212, float3(0.454545438289642333984375));
    }
    else
    {
        _217 = _212;
    }
    out.out_var_SV_Target0 = float4(_217, _146.w);
    return out;
}

