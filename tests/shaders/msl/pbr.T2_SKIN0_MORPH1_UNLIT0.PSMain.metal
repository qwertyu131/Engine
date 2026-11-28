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

constant float4 _140 = {};

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
};

fragment PSMain_out PSMain(PSMain_in in [[stage_in]], constant type_Frame& Frame [[buffer(0)]], constant type_Material& Material [[buffer(1)]], texturecube<float> specularEnvTexture [[texture(0)]], texturecube<float> irradianceEnvTexture [[texture(1)]], texture2d<float> brdfLutTexture [[texture(2)]], texture2d<float> baseColorTexture [[texture(3)]], texture2d<float> metallicRoughnessTexture [[texture(4)]], texture2d<float> normalTexture [[texture(5)]], texture2d<float> occlusionTexture [[texture(6)]], texture2d<float> emissiveTexture [[texture(7)]], sampler envSampler [[sampler(0)]], sampler brdfLutSampler [[sampler(1)]], sampler baseColorSampler [[sampler(2)]], sampler metallicRoughnessSampler [[sampler(3)]], sampler normalSampler [[sampler(4)]], sampler occlusionSampler [[sampler(5)]], sampler emissiveSampler [[sampler(6)]], bool gl_FrontFacing [[front_facing]])
{
    PSMain_out out = {};
    float2 _159 = select(in.in_var_TEXCOORD0, in.in_var_TEXCOORD1, bool2(Material.texCoordSets.x == 1u));
    float4 _174 = baseColorTexture.sample(baseColorSampler, float2(dot(Material.uvTransform[0u].xy, _159) + Material.uvTransform[0u].z, dot(Material.uvTransform[1u].xy, _159) + Material.uvTransform[1u].z));
    float4 _176 = (Material.baseColorFactor * _174) * in.in_var_COLOR0;
    float4 _183;
    if (Material.materialFlags.x == 0u)
    {
        float4 _182 = _176;
        _182.w = 1.0;
        _183 = _182;
    }
    else
    {
        _183 = _176;
    }
    float3 _188 = fast::normalize(Frame.cameraPosition.xyz - in.in_var_WORLD_POSITION);
    float3 _189 = fast::normalize(in.in_var_TANGENT);
    float3 _190 = fast::normalize(in.in_var_BINORMAL);
    float3 _191 = fast::normalize(in.in_var_NORMAL);
    float3 _198;
    float3 _199;
    float3 _200;
    if (!gl_FrontFacing)
    {
        _198 = -_190;
        _199 = -_189;
        _200 = -_191;
    }
    else
    {
        _198 = _190;
        _199 = _189;
        _200 = _191;
    }
    float2 _207 = select(in.in_var_TEXCOORD0, in.in_var_TEXCOORD1, bool2(Material.texCoordSets.z == 1u));
    float4 _222 = normalTexture.sample(normalSampler, float2(dot(Material.uvTransform[4u].xy, _207) + Material.uvTransform[4u].z, dot(Material.uvTransform[5u].xy, _207) + Material.uvTransform[5u].z));
    float3 _245;
    if (Material.materialFlags.z != 0u)
    {
        float3 _235 = fast::normalize(((_222.xyz * 2.0) - float3(1.0)) * float3(Material.pbrParams.z, Material.pbrParams.z, 1.0));
        _245 = fast::normalize(((_199 * _235.x) + (_198 * _235.y)) + (_200 * _235.z));
    }
    else
    {
        _245 = _200;
    }
    float2 _252 = select(in.in_var_TEXCOORD0, in.in_var_TEXCOORD1, bool2(Material.texCoordSets.y == 1u));
    float4 _267 = metallicRoughnessTexture.sample(metallicRoughnessSampler, float2(dot(Material.uvTransform[2u].xy, _252) + Material.uvTransform[2u].z, dot(Material.uvTransform[3u].xy, _252) + Material.uvTransform[3u].z));
    float _277 = fast::clamp(Material.pbrParams.y * _267.y, 0.0, 1.0);
    float3x3 _288 = float3x3(float4(Frame.envRotation[0][0], Frame.envRotation[1][0], Frame.envRotation[2][0], Frame.envRotation[3][0]).xyz, float4(Frame.envRotation[0][1], Frame.envRotation[1][1], Frame.envRotation[2][1], Frame.envRotation[3][1]).xyz, float4(Frame.envRotation[0][2], Frame.envRotation[1][2], Frame.envRotation[2][2], Frame.envRotation[3][2]).xyz);
    float4 _291 = irradianceEnvTexture.sample(envSampler, (_245 * _288));
    float3 _312 = (specularEnvTexture.sample(envSampler, (fast::normalize(reflect(-_188, _245)) * _288), level(_277 * (Frame.iblParams.z - 1.0))).xyz * Frame.iblParams.y).xyz;
    float _314 = fast::clamp(dot(_245, _188), 0.0, 1.0);
    float2 _316 = fast::clamp(float2(_314, _277), float2(0.0), float2(1.0));
    float4 _320 = brdfLutTexture.sample(brdfLutSampler, _316);
    float3 _322 = float3(1.0 - _277);
    float _326 = powr(1.0 - _314, 5.0);
    float _329 = _320.x;
    float _331 = _320.y;
    float3 _334 = (((_183.xyz + ((precise::max(_322, _183.xyz) - _183.xyz) * _326)) * _329) + float3(_331)) * 1.0;
    float _336 = 1.0 - (_329 + _331);
    float3 _340 = (_183.xyz + ((float3(1.0) - _183.xyz) * float3(0.0476190485060214996337890625))) * 1.0;
    float4 _351 = brdfLutTexture.sample(brdfLutSampler, _316);
    float _356 = _351.x;
    float _358 = _351.y;
    float3 _361 = (((float3(0.039999999105930328369140625) + ((precise::max(_322, float3(0.039999999105930328369140625)) - float3(0.039999999105930328369140625)) * _326)) * _356) + float3(_358)) * 1.0;
    float _363 = 1.0 - (_356 + _358);
    float2 _379 = select(in.in_var_TEXCOORD0, in.in_var_TEXCOORD1, bool2(Material.texCoordSets.w == 1u));
    float4 _394 = occlusionTexture.sample(occlusionSampler, float2(dot(Material.uvTransform[6u].xy, _379) + Material.uvTransform[6u].z, dot(Material.uvTransform[7u].xy, _379) + Material.uvTransform[7u].z));
    float2 _408 = select(in.in_var_TEXCOORD0, in.in_var_TEXCOORD1, bool2(Material.texCoordSets2.x == 1u));
    float4 _423 = emissiveTexture.sample(emissiveSampler, float2(dot(Material.uvTransform[8u].xy, _408) + Material.uvTransform[8u].z, dot(Material.uvTransform[9u].xy, _408) + Material.uvTransform[9u].z));
    float4 _440;
    if (Material.materialFlags.x == 1u)
    {
        if (_183.w < Material.alphaParams.x)
        {
            discard_fragment();
        }
        float4 _439;
        _439.w = 1.0;
        _440 = _439;
    }
    else
    {
        _440 = _183;
    }
    float3 _447 = ((Material.emissiveFactor.xyz * _423.xyz) + (mix(mix((_291.xyz * Frame.iblParams.y) * _183.xyz, _312, _361 + (((_361 * _363) * float3(0.085714280605316162109375)) / (float3(1.0) - (float3(0.085714280605316162109375) * _363)))), (_334 + (((_334 * _336) * _340) / (float3(1.0) - (_340 * _336)))) * _312, float3(fast::clamp(Material.pbrParams.x * _267.z, 0.0, 1.0))) * (1.0 + (Material.pbrParams.w * (_394.x - 1.0))))) * Frame.iblParams.x;
    float3 _506;
    if (Frame.frameFlags.y == 0u)
    {
        float3 _490;
        do
        {
            float _458 = precise::min(_447.x, precise::min(_447.y, _447.z));
            float _466;
            if (_458 < 0.07999999821186065673828125)
            {
                _466 = _458 - ((6.25 * _458) * _458);
            }
            else
            {
                _466 = 0.039999999105930328369140625;
            }
            float3 _468 = _447 - float3(_466);
            float _473 = precise::max(_468.x, precise::max(_468.y, _468.z));
            if (_473 < 0.7599999904632568359375)
            {
                _490 = _468;
                break;
            }
            float _479 = 1.0 - (0.0576000027358531951904296875 / (_473 + (-0.519999980926513671875)));
            _490 = mix(_468 * (_479 / _473), float3(1.0) * _479, float3(1.0 - (1.0 / ((0.1500000059604644775390625 * (_473 - _479)) + 1.0))));
            break;
        } while(false);
        _506 = _490;
    }
    else
    {
        float3 _505;
        if (Frame.frameFlags.y == 1u)
        {
            float3 _494 = float3x3(float3(0.59719002246856689453125, 0.075999997556209564208984375, 0.0284000001847743988037109375), float3(0.354579985141754150390625, 0.908339977264404296875, 0.13382999598979949951171875), float3(0.048229999840259552001953125, 0.0156599991023540496826171875, 0.837769985198974609375)) * _447;
            _505 = fast::clamp(float3x3(float3(1.60475003719329833984375, -0.10208000242710113525390625, -0.00326999998651444911956787109375), float3(-0.5310800075531005859375, 1.108129978179931640625, -0.07276000082492828369140625), float3(-0.0736699998378753662109375, -0.00604999996721744537353515625, 1.0760200023651123046875)) * (((_494 * (_494 + float3(0.02457859925925731658935546875))) - float3(9.0537003416102379560470581054688e-05)) / ((_494 * ((_494 * 0.98372900485992431640625) + float3(0.4329510033130645751953125))) + float3(0.23808099329471588134765625))), float3(0.0), float3(1.0));
        }
        else
        {
            _505 = _447;
        }
        _506 = _505;
    }
    float3 _511;
    if (Frame.frameFlags.x != 0u)
    {
        _511 = powr(_506, float3(0.454545438289642333984375));
    }
    else
    {
        _511 = _506;
    }
    out.out_var_SV_Target0 = float4(_511, _440.w);
    return out;
}

