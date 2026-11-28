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

constant float4 _143 = {};

struct PSMain_out
{
    float4 out_var_SV_Target0 [[color(0)]];
};

struct PSMain_in
{
    float3 in_var_WORLD_POSITION [[user(locn0)]];
    float3 in_var_TANGENT [[user(locn1)]];
    float3 in_var_BINORMAL [[user(locn2)]];
    float3 in_var_NORMAL [[user(locn3)]];
    float2 in_var_TEXCOORD0 [[user(locn4)]];
    float2 in_var_TEXCOORD1 [[user(locn5)]];
    float4 in_var_COLOR0 [[user(locn6)]];
    uint in_var_DRAWFLAGS [[user(locn7)]];
};

fragment PSMain_out PSMain(PSMain_in in [[stage_in]], constant type_Frame& Frame [[buffer(0)]], constant type_Material& Material [[buffer(1)]], texturecube<float> specularEnvTexture [[texture(0)]], texturecube<float> irradianceEnvTexture [[texture(1)]], texture2d<float> brdfLutTexture [[texture(2)]], texture2d<float> baseColorTexture [[texture(3)]], texture2d<float> metallicRoughnessTexture [[texture(4)]], texture2d<float> normalTexture [[texture(5)]], texture2d<float> occlusionTexture [[texture(6)]], texture2d<float> emissiveTexture [[texture(7)]], sampler envSampler [[sampler(0)]], sampler brdfLutSampler [[sampler(1)]], sampler baseColorSampler [[sampler(2)]], sampler metallicRoughnessSampler [[sampler(3)]], sampler normalSampler [[sampler(4)]], sampler occlusionSampler [[sampler(5)]], sampler emissiveSampler [[sampler(6)]], bool gl_FrontFacing [[front_facing]])
{
    PSMain_out out = {};
    float2 _163 = select(in.in_var_TEXCOORD0, in.in_var_TEXCOORD1, bool2(Material.texCoordSets.x == 1u));
    float4 _178 = baseColorTexture.sample(baseColorSampler, float2(dot(Material.uvTransform[0u].xy, _163) + Material.uvTransform[0u].z, dot(Material.uvTransform[1u].xy, _163) + Material.uvTransform[1u].z));
    float4 _180 = (Material.baseColorFactor * _178) * in.in_var_COLOR0;
    float4 _187;
    if (Material.materialFlags.x == 0u)
    {
        float4 _186 = _180;
        _186.w = 1.0;
        _187 = _186;
    }
    else
    {
        _187 = _180;
    }
    float3 _192 = fast::normalize(Frame.cameraPosition.xyz - in.in_var_WORLD_POSITION);
    float3 _193 = fast::normalize(in.in_var_TANGENT);
    float3 _194 = fast::normalize(in.in_var_BINORMAL);
    float3 _195 = fast::normalize(in.in_var_NORMAL);
    float3 _202;
    float3 _203;
    float3 _204;
    if (!gl_FrontFacing)
    {
        _202 = -_194;
        _203 = -_193;
        _204 = -_195;
    }
    else
    {
        _202 = _194;
        _203 = _193;
        _204 = _195;
    }
    float2 _211 = select(in.in_var_TEXCOORD0, in.in_var_TEXCOORD1, bool2(Material.texCoordSets.z == 1u));
    float4 _226 = normalTexture.sample(normalSampler, float2(dot(Material.uvTransform[4u].xy, _211) + Material.uvTransform[4u].z, dot(Material.uvTransform[5u].xy, _211) + Material.uvTransform[5u].z));
    bool _235;
    if (Material.materialFlags.z != 0u)
    {
        _235 = (in.in_var_DRAWFLAGS & 2u) == 0u;
    }
    else
    {
        _235 = false;
    }
    float3 _254;
    if (_235)
    {
        float3 _244 = fast::normalize(((_226.xyz * 2.0) - float3(1.0)) * float3(Material.pbrParams.z, Material.pbrParams.z, 1.0));
        _254 = fast::normalize(((_203 * _244.x) + (_202 * _244.y)) + (_204 * _244.z));
    }
    else
    {
        _254 = _204;
    }
    float2 _261 = select(in.in_var_TEXCOORD0, in.in_var_TEXCOORD1, bool2(Material.texCoordSets.y == 1u));
    float4 _276 = metallicRoughnessTexture.sample(metallicRoughnessSampler, float2(dot(Material.uvTransform[2u].xy, _261) + Material.uvTransform[2u].z, dot(Material.uvTransform[3u].xy, _261) + Material.uvTransform[3u].z));
    float _286 = fast::clamp(Material.pbrParams.y * _276.y, 0.0, 1.0);
    float3x3 _297 = float3x3(float4(Frame.envRotation[0][0], Frame.envRotation[1][0], Frame.envRotation[2][0], Frame.envRotation[3][0]).xyz, float4(Frame.envRotation[0][1], Frame.envRotation[1][1], Frame.envRotation[2][1], Frame.envRotation[3][1]).xyz, float4(Frame.envRotation[0][2], Frame.envRotation[1][2], Frame.envRotation[2][2], Frame.envRotation[3][2]).xyz);
    float4 _300 = irradianceEnvTexture.sample(envSampler, (_254 * _297));
    float3 _321 = (specularEnvTexture.sample(envSampler, (fast::normalize(reflect(-_192, _254)) * _297), level(_286 * (Frame.iblParams.z - 1.0))).xyz * Frame.iblParams.y).xyz;
    float _323 = fast::clamp(dot(_254, _192), 0.0, 1.0);
    float2 _325 = fast::clamp(float2(_323, _286), float2(0.0), float2(1.0));
    float4 _329 = brdfLutTexture.sample(brdfLutSampler, _325);
    float3 _331 = float3(1.0 - _286);
    float _335 = powr(1.0 - _323, 5.0);
    float _338 = _329.x;
    float _340 = _329.y;
    float3 _343 = (((_187.xyz + ((precise::max(_331, _187.xyz) - _187.xyz) * _335)) * _338) + float3(_340)) * 1.0;
    float _345 = 1.0 - (_338 + _340);
    float3 _349 = (_187.xyz + ((float3(1.0) - _187.xyz) * float3(0.0476190485060214996337890625))) * 1.0;
    float4 _360 = brdfLutTexture.sample(brdfLutSampler, _325);
    float _365 = _360.x;
    float _367 = _360.y;
    float3 _370 = (((float3(0.039999999105930328369140625) + ((precise::max(_331, float3(0.039999999105930328369140625)) - float3(0.039999999105930328369140625)) * _335)) * _365) + float3(_367)) * 1.0;
    float _372 = 1.0 - (_365 + _367);
    float2 _388 = select(in.in_var_TEXCOORD0, in.in_var_TEXCOORD1, bool2(Material.texCoordSets.w == 1u));
    float4 _403 = occlusionTexture.sample(occlusionSampler, float2(dot(Material.uvTransform[6u].xy, _388) + Material.uvTransform[6u].z, dot(Material.uvTransform[7u].xy, _388) + Material.uvTransform[7u].z));
    float2 _417 = select(in.in_var_TEXCOORD0, in.in_var_TEXCOORD1, bool2(Material.texCoordSets2.x == 1u));
    float4 _432 = emissiveTexture.sample(emissiveSampler, float2(dot(Material.uvTransform[8u].xy, _417) + Material.uvTransform[8u].z, dot(Material.uvTransform[9u].xy, _417) + Material.uvTransform[9u].z));
    float3 _437 = Material.emissiveFactor.xyz * _432.xyz;
    float3 _444;
    if ((in.in_var_DRAWFLAGS & 1u) != 0u)
    {
        _444 = _437 + _187.xyz;
    }
    else
    {
        _444 = _437 + (mix(mix((_300.xyz * Frame.iblParams.y) * _187.xyz, _321, _370 + (((_370 * _372) * float3(0.085714280605316162109375)) / (float3(1.0) - (float3(0.085714280605316162109375) * _372)))), (_343 + (((_343 * _345) * _349) / (float3(1.0) - (_349 * _345)))) * _321, float3(fast::clamp(Material.pbrParams.x * _276.z, 0.0, 1.0))) * (1.0 + (Material.pbrParams.w * (_403.x - 1.0))));
    }
    float4 _455;
    if (Material.materialFlags.x == 1u)
    {
        if (_187.w < Material.alphaParams.x)
        {
            discard_fragment();
        }
        float4 _454;
        _454.w = 1.0;
        _455 = _454;
    }
    else
    {
        _455 = _187;
    }
    float3 _462 = _444 * Frame.iblParams.x;
    float3 _521;
    if (Frame.frameFlags.y == 0u)
    {
        float3 _505;
        do
        {
            float _473 = precise::min(_462.x, precise::min(_462.y, _462.z));
            float _481;
            if (_473 < 0.07999999821186065673828125)
            {
                _481 = _473 - ((6.25 * _473) * _473);
            }
            else
            {
                _481 = 0.039999999105930328369140625;
            }
            float3 _483 = _462 - float3(_481);
            float _488 = precise::max(_483.x, precise::max(_483.y, _483.z));
            if (_488 < 0.7599999904632568359375)
            {
                _505 = _483;
                break;
            }
            float _494 = 1.0 - (0.0576000027358531951904296875 / (_488 + (-0.519999980926513671875)));
            _505 = mix(_483 * (_494 / _488), float3(1.0) * _494, float3(1.0 - (1.0 / ((0.1500000059604644775390625 * (_488 - _494)) + 1.0))));
            break;
        } while(false);
        _521 = _505;
    }
    else
    {
        float3 _520;
        if (Frame.frameFlags.y == 1u)
        {
            float3 _509 = float3x3(float3(0.59719002246856689453125, 0.075999997556209564208984375, 0.0284000001847743988037109375), float3(0.354579985141754150390625, 0.908339977264404296875, 0.13382999598979949951171875), float3(0.048229999840259552001953125, 0.0156599991023540496826171875, 0.837769985198974609375)) * _462;
            _520 = fast::clamp(float3x3(float3(1.60475003719329833984375, -0.10208000242710113525390625, -0.00326999998651444911956787109375), float3(-0.5310800075531005859375, 1.108129978179931640625, -0.07276000082492828369140625), float3(-0.0736699998378753662109375, -0.00604999996721744537353515625, 1.0760200023651123046875)) * (((_509 * (_509 + float3(0.02457859925925731658935546875))) - float3(9.0537003416102379560470581054688e-05)) / ((_509 * ((_509 * 0.98372900485992431640625) + float3(0.4329510033130645751953125))) + float3(0.23808099329471588134765625))), float3(0.0), float3(1.0));
        }
        else
        {
            _520 = _462;
        }
        _521 = _520;
    }
    float3 _526;
    if (Frame.frameFlags.x != 0u)
    {
        _526 = powr(_521, float3(0.454545438289642333984375));
    }
    else
    {
        _526 = _521;
    }
    out.out_var_SV_Target0 = float4(_526, _455.w);
    return out;
}

