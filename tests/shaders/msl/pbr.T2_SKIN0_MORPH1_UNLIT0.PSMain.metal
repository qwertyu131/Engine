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

constant float4 _104 = {};

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
    float2 _123 = select(in.in_var_TEXCOORD0, in.in_var_TEXCOORD1, bool2(Material.texCoordSets.x == 1u));
    float4 _138 = baseColorTexture.sample(baseColorSampler, float2(dot(Material.uvTransform[0u].xy, _123) + Material.uvTransform[0u].z, dot(Material.uvTransform[1u].xy, _123) + Material.uvTransform[1u].z));
    float4 _140 = (Material.baseColorFactor * _138) * in.in_var_COLOR0;
    float4 _147;
    if (Material.materialFlags.x == 0u)
    {
        float4 _146 = _140;
        _146.w = 1.0;
        _147 = _146;
    }
    else
    {
        _147 = _140;
    }
    float3 _152 = fast::normalize(Frame.cameraPosition.xyz - in.in_var_WORLD_POSITION);
    float3 _153 = fast::normalize(in.in_var_TANGENT);
    float3 _154 = fast::normalize(in.in_var_BINORMAL);
    float3 _155 = fast::normalize(in.in_var_NORMAL);
    float3 _162;
    float3 _163;
    float3 _164;
    if (!gl_FrontFacing)
    {
        _162 = -_154;
        _163 = -_153;
        _164 = -_155;
    }
    else
    {
        _162 = _154;
        _163 = _153;
        _164 = _155;
    }
    float2 _171 = select(in.in_var_TEXCOORD0, in.in_var_TEXCOORD1, bool2(Material.texCoordSets.z == 1u));
    float4 _186 = normalTexture.sample(normalSampler, float2(dot(Material.uvTransform[4u].xy, _171) + Material.uvTransform[4u].z, dot(Material.uvTransform[5u].xy, _171) + Material.uvTransform[5u].z));
    float3 _209;
    if (Material.materialFlags.z != 0u)
    {
        float3 _199 = fast::normalize(((_186.xyz * 2.0) - float3(1.0)) * float3(Material.pbrParams.z, Material.pbrParams.z, 1.0));
        _209 = fast::normalize(((_163 * _199.x) + (_162 * _199.y)) + (_164 * _199.z));
    }
    else
    {
        _209 = _164;
    }
    float2 _216 = select(in.in_var_TEXCOORD0, in.in_var_TEXCOORD1, bool2(Material.texCoordSets.y == 1u));
    float4 _231 = metallicRoughnessTexture.sample(metallicRoughnessSampler, float2(dot(Material.uvTransform[2u].xy, _216) + Material.uvTransform[2u].z, dot(Material.uvTransform[3u].xy, _216) + Material.uvTransform[3u].z));
    float _241 = fast::clamp(Material.pbrParams.y * _231.y, 0.0, 1.0);
    float3x3 _252 = float3x3(float4(Frame.envRotation[0][0], Frame.envRotation[1][0], Frame.envRotation[2][0], Frame.envRotation[3][0]).xyz, float4(Frame.envRotation[0][1], Frame.envRotation[1][1], Frame.envRotation[2][1], Frame.envRotation[3][1]).xyz, float4(Frame.envRotation[0][2], Frame.envRotation[1][2], Frame.envRotation[2][2], Frame.envRotation[3][2]).xyz);
    float4 _255 = irradianceEnvTexture.sample(envSampler, (_209 * _252));
    float3 _276 = (specularEnvTexture.sample(envSampler, (fast::normalize(reflect(-_152, _209)) * _252), level(_241 * (Frame.iblParams.z - 1.0))).xyz * Frame.iblParams.y).xyz;
    float _278 = fast::clamp(dot(_209, _152), 0.0, 1.0);
    float2 _280 = fast::clamp(float2(_278, _241), float2(0.0), float2(1.0));
    float4 _284 = brdfLutTexture.sample(brdfLutSampler, _280);
    float3 _286 = float3(1.0 - _241);
    float _290 = powr(1.0 - _278, 5.0);
    float _293 = _284.x;
    float _295 = _284.y;
    float3 _298 = (((_147.xyz + ((precise::max(_286, _147.xyz) - _147.xyz) * _290)) * _293) + float3(_295)) * 1.0;
    float _300 = 1.0 - (_293 + _295);
    float3 _304 = (_147.xyz + ((float3(1.0) - _147.xyz) * float3(0.0476190485060214996337890625))) * 1.0;
    float4 _315 = brdfLutTexture.sample(brdfLutSampler, _280);
    float _320 = _315.x;
    float _322 = _315.y;
    float3 _325 = (((float3(0.039999999105930328369140625) + ((precise::max(_286, float3(0.039999999105930328369140625)) - float3(0.039999999105930328369140625)) * _290)) * _320) + float3(_322)) * 1.0;
    float _327 = 1.0 - (_320 + _322);
    float2 _343 = select(in.in_var_TEXCOORD0, in.in_var_TEXCOORD1, bool2(Material.texCoordSets.w == 1u));
    float4 _358 = occlusionTexture.sample(occlusionSampler, float2(dot(Material.uvTransform[6u].xy, _343) + Material.uvTransform[6u].z, dot(Material.uvTransform[7u].xy, _343) + Material.uvTransform[7u].z));
    float2 _372 = select(in.in_var_TEXCOORD0, in.in_var_TEXCOORD1, bool2(Material.texCoordSets2.x == 1u));
    float4 _387 = emissiveTexture.sample(emissiveSampler, float2(dot(Material.uvTransform[8u].xy, _372) + Material.uvTransform[8u].z, dot(Material.uvTransform[9u].xy, _372) + Material.uvTransform[9u].z));
    float4 _404;
    if (Material.materialFlags.x == 1u)
    {
        if (_147.w < Material.alphaParams.x)
        {
            discard_fragment();
        }
        float4 _403;
        _403.w = 1.0;
        _404 = _403;
    }
    else
    {
        _404 = _147;
    }
    float3 _409 = ((Material.emissiveFactor.xyz * _387.xyz) + (mix(mix((_255.xyz * Frame.iblParams.y) * _147.xyz, _276, _325 + (((_325 * _327) * float3(0.085714280605316162109375)) / (float3(1.0) - (float3(0.085714280605316162109375) * _327)))), (_298 + (((_298 * _300) * _304) / (float3(1.0) - (_304 * _300)))) * _276, float3(fast::clamp(Material.pbrParams.x * _231.z, 0.0, 1.0))) * (1.0 + (Material.pbrParams.w * (_358.x - 1.0))))) * Frame.iblParams.x;
    float3 _448;
    do
    {
        float _416 = precise::min(_409.x, precise::min(_409.y, _409.z));
        float _424;
        if (_416 < 0.07999999821186065673828125)
        {
            _424 = _416 - ((6.25 * _416) * _416);
        }
        else
        {
            _424 = 0.039999999105930328369140625;
        }
        float3 _426 = _409 - float3(_424);
        float _431 = precise::max(_426.x, precise::max(_426.y, _426.z));
        if (_431 < 0.7599999904632568359375)
        {
            _448 = _426;
            break;
        }
        float _437 = 1.0 - (0.0576000027358531951904296875 / (_431 + (-0.519999980926513671875)));
        _448 = mix(_426 * (_437 / _431), float3(1.0) * _437, float3(1.0 - (1.0 / ((0.1500000059604644775390625 * (_431 - _437)) + 1.0))));
        break;
    } while(false);
    float3 _453;
    if (Frame.frameFlags.x != 0u)
    {
        _453 = powr(_448, float3(0.4545454680919647216796875));
    }
    else
    {
        _453 = _448;
    }
    out.out_var_SV_Target0 = float4(_453, _404.w);
    return out;
}

