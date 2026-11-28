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

constant float4 _99 = {};

struct PSMain_out
{
    float4 out_var_SV_Target0 [[color(0)]];
};

struct PSMain_in
{
    float2 in_var_TEXCOORD0 [[user(locn4)]];
    float2 in_var_TEXCOORD1 [[user(locn5)]];
    float4 in_var_COLOR0 [[user(locn6)]];
    uint in_var_DRAWFLAGS [[user(locn7)]];
};

fragment PSMain_out PSMain(PSMain_in in [[stage_in]], constant type_Frame& Frame [[buffer(0)]], constant type_Material& Material [[buffer(1)]], texture2d<float> baseColorTexture [[texture(0)]], sampler baseColorSampler [[sampler(0)]])
{
    PSMain_out out = {};
    float2 _114 = select(in.in_var_TEXCOORD0, in.in_var_TEXCOORD1, bool2(Material.texCoordSets.x == 1u));
    float4 _129 = baseColorTexture.sample(baseColorSampler, float2(dot(Material.uvTransform[0u].xy, _114) + Material.uvTransform[0u].z, dot(Material.uvTransform[1u].xy, _114) + Material.uvTransform[1u].z));
    float4 _131 = (Material.baseColorFactor * _129) * in.in_var_COLOR0;
    float4 _138;
    if (Material.materialFlags.x == 0u)
    {
        float4 _137 = _131;
        _137.w = 1.0;
        _138 = _137;
    }
    else
    {
        _138 = _131;
    }
    if ((in.in_var_DRAWFLAGS & 2147483648u) != 0u)
    {
        discard_fragment();
    }
    float4 _154;
    if (Material.materialFlags.x == 1u)
    {
        if (_138.w < Material.alphaParams.x)
        {
            discard_fragment();
        }
        float4 _153;
        _153.w = 1.0;
        _154 = _153;
    }
    else
    {
        _154 = _138;
    }
    float3 _161 = _138.xyz * Frame.iblParams.x;
    float3 _220;
    if (Frame.frameFlags.y == 0u)
    {
        float3 _204;
        do
        {
            float _172 = precise::min(_161.x, precise::min(_161.y, _161.z));
            float _180;
            if (_172 < 0.07999999821186065673828125)
            {
                _180 = _172 - ((6.25 * _172) * _172);
            }
            else
            {
                _180 = 0.039999999105930328369140625;
            }
            float3 _182 = _161 - float3(_180);
            float _187 = precise::max(_182.x, precise::max(_182.y, _182.z));
            if (_187 < 0.7599999904632568359375)
            {
                _204 = _182;
                break;
            }
            float _193 = 1.0 - (0.0576000027358531951904296875 / (_187 + (-0.519999980926513671875)));
            _204 = mix(_182 * (_193 / _187), float3(1.0) * _193, float3(1.0 - (1.0 / ((0.1500000059604644775390625 * (_187 - _193)) + 1.0))));
            break;
        } while(false);
        _220 = _204;
    }
    else
    {
        float3 _219;
        if (Frame.frameFlags.y == 1u)
        {
            float3 _208 = float3x3(float3(0.59719002246856689453125, 0.075999997556209564208984375, 0.0284000001847743988037109375), float3(0.354579985141754150390625, 0.908339977264404296875, 0.13382999598979949951171875), float3(0.048229999840259552001953125, 0.0156599991023540496826171875, 0.837769985198974609375)) * _161;
            _219 = fast::clamp(float3x3(float3(1.60475003719329833984375, -0.10208000242710113525390625, -0.00326999998651444911956787109375), float3(-0.5310800075531005859375, 1.108129978179931640625, -0.07276000082492828369140625), float3(-0.0736699998378753662109375, -0.00604999996721744537353515625, 1.0760200023651123046875)) * (((_208 * (_208 + float3(0.02457859925925731658935546875))) - float3(9.0537003416102379560470581054688e-05)) / ((_208 * ((_208 * 0.98372900485992431640625) + float3(0.4329510033130645751953125))) + float3(0.23808099329471588134765625))), float3(0.0), float3(1.0));
        }
        else
        {
            _219 = _161;
        }
        _220 = _219;
    }
    float3 _225;
    if (Frame.frameFlags.x != 0u)
    {
        _225 = powr(_220, float3(0.454545438289642333984375));
    }
    else
    {
        _225 = _220;
    }
    out.out_var_SV_Target0 = float4(_225, _154.w);
    return out;
}

