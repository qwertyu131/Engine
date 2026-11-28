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

struct type_Lights
{
    uint4 lightInfo;
    float4 shadowInfo;
    float4 viewForward;
    float4 lights[256];
};

struct type_Shadows
{
    float4x4 shadowMatrices[64];
    float4 shadowRects[64];
    float4 shadowParams[64];
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

constant float4 _169 = {};

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

fragment PSMain_out PSMain(PSMain_in in [[stage_in]], constant type_Frame& Frame [[buffer(0)]], constant type_Lights& Lights [[buffer(1)]], constant type_Shadows& Shadows [[buffer(2)]], constant type_Material& Material [[buffer(3)]], texturecube<float> specularEnvTexture [[texture(0)]], texturecube<float> irradianceEnvTexture [[texture(1)]], texture2d<float> brdfLutTexture [[texture(2)]], texture2d<float> shadowAtlas [[texture(3)]], texture2d<float> baseColorTexture [[texture(4)]], texture2d<float> metallicRoughnessTexture [[texture(5)]], texture2d<float> normalTexture [[texture(6)]], texture2d<float> occlusionTexture [[texture(7)]], texture2d<float> emissiveTexture [[texture(8)]], sampler envSampler [[sampler(0)]], sampler brdfLutSampler [[sampler(1)]], sampler shadowSampler [[sampler(2)]], sampler baseColorSampler [[sampler(3)]], sampler metallicRoughnessSampler [[sampler(4)]], sampler normalSampler [[sampler(5)]], sampler occlusionSampler [[sampler(6)]], sampler emissiveSampler [[sampler(7)]], bool gl_FrontFacing [[front_facing]])
{
    PSMain_out out = {};
    float2 _189 = select(in.in_var_TEXCOORD0, in.in_var_TEXCOORD1, bool2(Material.texCoordSets.x == 1u));
    float4 _204 = baseColorTexture.sample(baseColorSampler, float2(dot(Material.uvTransform[0u].xy, _189) + Material.uvTransform[0u].z, dot(Material.uvTransform[1u].xy, _189) + Material.uvTransform[1u].z));
    float4 _206 = (Material.baseColorFactor * _204) * in.in_var_COLOR0;
    float4 _213;
    if (Material.materialFlags.x == 0u)
    {
        float4 _212 = _206;
        _212.w = 1.0;
        _213 = _212;
    }
    else
    {
        _213 = _206;
    }
    float3 _218 = fast::normalize(Frame.cameraPosition.xyz - in.in_var_WORLD_POSITION);
    float3 _219 = fast::normalize(in.in_var_TANGENT);
    float3 _220 = fast::normalize(in.in_var_BINORMAL);
    float3 _221 = fast::normalize(in.in_var_NORMAL);
    float3 _228;
    float3 _229;
    float3 _230;
    if (!gl_FrontFacing)
    {
        _228 = -_220;
        _229 = -_219;
        _230 = -_221;
    }
    else
    {
        _228 = _220;
        _229 = _219;
        _230 = _221;
    }
    float2 _237 = select(in.in_var_TEXCOORD0, in.in_var_TEXCOORD1, bool2(Material.texCoordSets.z == 1u));
    float4 _252 = normalTexture.sample(normalSampler, float2(dot(Material.uvTransform[4u].xy, _237) + Material.uvTransform[4u].z, dot(Material.uvTransform[5u].xy, _237) + Material.uvTransform[5u].z));
    bool _261;
    if (Material.materialFlags.z != 0u)
    {
        _261 = (in.in_var_DRAWFLAGS & 2u) == 0u;
    }
    else
    {
        _261 = false;
    }
    float3 _280;
    if (_261)
    {
        float3 _270 = fast::normalize(((_252.xyz * 2.0) - float3(1.0)) * float3(Material.pbrParams.z, Material.pbrParams.z, 1.0));
        _280 = fast::normalize(((_229 * _270.x) + (_228 * _270.y)) + (_230 * _270.z));
    }
    else
    {
        _280 = _230;
    }
    float2 _287 = select(in.in_var_TEXCOORD0, in.in_var_TEXCOORD1, bool2(Material.texCoordSets.y == 1u));
    float4 _302 = metallicRoughnessTexture.sample(metallicRoughnessSampler, float2(dot(Material.uvTransform[2u].xy, _287) + Material.uvTransform[2u].z, dot(Material.uvTransform[3u].xy, _287) + Material.uvTransform[3u].z));
    float _312 = fast::clamp(Material.pbrParams.y * _302.y, 0.0, 1.0);
    float3x3 _323 = float3x3(float4(Frame.envRotation[0][0], Frame.envRotation[1][0], Frame.envRotation[2][0], Frame.envRotation[3][0]).xyz, float4(Frame.envRotation[0][1], Frame.envRotation[1][1], Frame.envRotation[2][1], Frame.envRotation[3][1]).xyz, float4(Frame.envRotation[0][2], Frame.envRotation[1][2], Frame.envRotation[2][2], Frame.envRotation[3][2]).xyz);
    float4 _326 = irradianceEnvTexture.sample(envSampler, (_280 * _323));
    float3 _347 = (specularEnvTexture.sample(envSampler, (fast::normalize(reflect(-_218, _280)) * _323), level(_312 * (Frame.iblParams.z - 1.0))).xyz * Frame.iblParams.y).xyz;
    float _349 = fast::clamp(dot(_280, _218), 0.0, 1.0);
    float2 _351 = fast::clamp(float2(_349, _312), float2(0.0), float2(1.0));
    float4 _355 = brdfLutTexture.sample(brdfLutSampler, _351);
    float3 _357 = float3(1.0 - _312);
    float _361 = powr(1.0 - _349, 5.0);
    float _364 = _355.x;
    float _366 = _355.y;
    float3 _369 = (((_213.xyz + ((precise::max(_357, _213.xyz) - _213.xyz) * _361)) * _364) + float3(_366)) * 1.0;
    float _371 = 1.0 - (_364 + _366);
    float3 _372 = float3(1.0) - _213.xyz;
    float3 _375 = (_213.xyz + (_372 * float3(0.0476190485060214996337890625))) * 1.0;
    float4 _386 = brdfLutTexture.sample(brdfLutSampler, _351);
    float _391 = _386.x;
    float _393 = _386.y;
    float3 _396 = (((float3(0.039999999105930328369140625) + ((precise::max(_357, float3(0.039999999105930328369140625)) - float3(0.039999999105930328369140625)) * _361)) * _391) + float3(_393)) * 1.0;
    float _398 = 1.0 - (_391 + _393);
    float3 _406 = float3(fast::clamp(Material.pbrParams.x * _302.z, 0.0, 1.0));
    float2 _414 = select(in.in_var_TEXCOORD0, in.in_var_TEXCOORD1, bool2(Material.texCoordSets.w == 1u));
    float4 _429 = occlusionTexture.sample(occlusionSampler, float2(dot(Material.uvTransform[6u].xy, _414) + Material.uvTransform[6u].z, dot(Material.uvTransform[7u].xy, _414) + Material.uvTransform[7u].z));
    float _437 = _312 * _312;
    uint _440 = min(Lights.lightInfo.x, 64u);
    float3 _442;
    _442 = float3(0.0);
    float3 _443;
    for (uint _445 = 0u; _445 < _440; _442 = _443, _445++)
    {
        uint _450 = 4u * _445;
        uint _453 = _450 + 2u;
        uint _456 = _450 + 3u;
        float3 _469;
        if (uint(Lights.lights[_456].y) != 0u)
        {
            _469 = Lights.lights[_453].xyz - in.in_var_WORLD_POSITION;
        }
        else
        {
            _469 = -Lights.lights[_450].xyz;
        }
        float3 _470 = fast::normalize(_469);
        float3 _472 = fast::normalize(_470 + _218);
        float _474 = fast::clamp(dot(_280, _470), 0.0, 1.0);
        float _476 = fast::clamp(dot(_280, _472), 0.0, 1.0);
        float _482 = powr(fast::clamp(1.0 - abs(fast::clamp(dot(_218, _472), 0.0, 1.0)), 0.0, 1.0), 5.0);
        uint _487 = _450 + 1u;
        uint _492 = uint(Lights.lights[_456].y);
        float _513;
        if (_492 != 0u)
        {
            float _497 = length(_469);
            float _512;
            do
            {
                if (Lights.lights[_450].w <= 0.0)
                {
                    _512 = 1.0 / powr(_497, 2.0);
                    break;
                }
                _512 = precise::max(precise::min(1.0 - powr(_497 / Lights.lights[_450].w, 4.0), 1.0), 0.0) / powr(_497, 2.0);
                break;
            } while(false);
            _513 = _512;
        }
        else
        {
            _513 = 1.0;
        }
        float _537;
        if (_492 == 2u)
        {
            float _536;
            do
            {
                float _525 = dot(fast::normalize(Lights.lights[_450].xyz), fast::normalize(-_469));
                if (_525 > Lights.lights[_456].x)
                {
                    if (_525 < Lights.lights[_453].w)
                    {
                        float _534 = (_525 - Lights.lights[_456].x) / (Lights.lights[_453].w - Lights.lights[_456].x);
                        _536 = _534 * _534;
                        break;
                    }
                    _536 = 1.0;
                    break;
                }
                _536 = 0.0;
                break;
            } while(false);
            _537 = _536;
        }
        else
        {
            _537 = 1.0;
        }
        float3 _543 = (Lights.lights[_487].xyz * ((_513 * _537) * Lights.lights[_487].w)) * _474;
        float3 _581;
        if (Lights.lightInfo.y != 0u)
        {
            float _553;
            float _570;
            do
            {
                _553 = _437 * _437;
                float _555 = 1.0 - _553;
                float _565 = (_474 * sqrt(((_349 * _349) * _555) + _553)) + (_349 * sqrt(((_474 * _474) * _555) + _553));
                if (_565 > 0.0)
                {
                    _570 = 0.5 / _565;
                    break;
                }
                _570 = 0.0;
                break;
            } while(false);
            float _574 = ((_476 * _476) * (_553 - 1.0)) + 1.0;
            _581 = _543 * float3(_570 * (_553 / ((3.1415927410125732421875 * _574) * _574)));
        }
        else
        {
            _581 = float3(0.0);
        }
        uint _588 = uint(precise::max(Lights.lights[_456].z, 0.0));
        uint _591 = uint(precise::max(Lights.lights[_456].w, 1.0));
        bool _592 = Lights.lights[_456].z >= 0.0;
        uint _659;
        bool _660;
        if (_492 == 0u)
        {
            float _601 = dot(in.in_var_WORLD_POSITION - Frame.cameraPosition.xyz, Lights.viewForward.xyz);
            uint _603;
            bool _606;
            _603 = _588;
            _606 = false;
            bool _607;
            uint _604;
            for (uint _608 = 0u; _608 < _591; _603 = _604, _606 = _607, _608++)
            {
                bool _613 = !_606;
                bool _620;
                if (_613)
                {
                    _620 = _601 <= Shadows.shadowRects[_588 + _608].w;
                }
                else
                {
                    _620 = false;
                }
                if (_620)
                {
                    _604 = _588 + _608;
                }
                else
                {
                    _604 = _603;
                }
                _607 = _613 ? _620 : true;
            }
            _659 = _603;
            _660 = _592 ? _606 : false;
        }
        else
        {
            uint _658;
            if (_492 == 1u)
            {
                float3 _629 = in.in_var_WORLD_POSITION - Lights.lights[_453].xyz;
                float3 _630 = abs(_629);
                float _640 = _630.x;
                float _641 = _630.y;
                bool _647;
                if (_640 >= _641)
                {
                    _647 = _640 >= _630.z;
                }
                else
                {
                    _647 = false;
                }
                uint _654;
                if (_647)
                {
                    _654 = (_629.x >= 0.0) ? 0u : 1u;
                }
                else
                {
                    _654 = (_641 >= _630.z) ? ((_629.y >= 0.0) ? 2u : 3u) : ((_629.z >= 0.0) ? 4u : 5u);
                }
                _658 = _588 + min(_654, (_591 - 1u));
            }
            else
            {
                _658 = _588;
            }
            _659 = _658;
            _660 = _592;
        }
        float _829;
        if (_660)
        {
            bool _668 = Shadows.shadowParams[_659].y > 0.5;
            float _678;
            if (_668)
            {
                _678 = Shadows.shadowParams[_659].x * precise::max(length(in.in_var_WORLD_POSITION - Lights.lights[_453].xyz), 9.9999997473787516355514526367188e-05);
            }
            else
            {
                _678 = Shadows.shadowParams[_659].x;
            }
            float4 _691 = Shadows.shadowMatrices[_659] * float4(in.in_var_WORLD_POSITION + (_230 * (Lights.shadowInfo.x * _678)), 1.0);
            float _693 = _691.w;
            float3 _697 = _691.xyz / float3(precise::max(abs(_693), 9.9999996826552253889678874634872e-21));
            bool _703;
            if (_693 > 0.0)
            {
                _703 = _697.z >= 0.0;
            }
            else
            {
                _703 = false;
            }
            bool _708;
            if (_703)
            {
                _708 = _697.z <= 1.0;
            }
            else
            {
                _708 = false;
            }
            bool _714;
            if (_708)
            {
                _714 = abs(_697.x) <= 1.0;
            }
            else
            {
                _714 = false;
            }
            bool _720;
            if (_714)
            {
                _720 = abs(_697.y) <= 1.0;
            }
            else
            {
                _720 = false;
            }
            float _722 = fast::clamp(_697.z, 0.0, 1.0);
            float _740;
            do
            {
                if (_668)
                {
                    float _729 = Shadows.shadowParams[_659].z - Shadows.shadowParams[_659].w;
                    _740 = ((Shadows.shadowParams[_659].z * Shadows.shadowParams[_659].w) / _729) / (_722 + (Shadows.shadowParams[_659].w / _729));
                    break;
                }
                _740 = Shadows.shadowParams[_659].z + (_722 * (Shadows.shadowParams[_659].w - Shadows.shadowParams[_659].z));
                break;
            } while(false);
            float _744 = _740 - (Lights.shadowInfo.y * _678);
            int2 _758 = int2(Shadows.shadowRects[_659].xy);
            int2 _762 = (_758 + int2(Shadows.shadowRects[_659].zz)) - int2(1);
            int2 _765 = clamp(int2(floor(Shadows.shadowRects[_659].xy + (float2(fast::clamp((_697.x * 0.5) + 0.5, 0.0, 1.0), fast::clamp(0.5 - (_697.y * 0.5), 0.0, 1.0)) * Shadows.shadowRects[_659].z))), _758, _762);
            int _769 = int(Lights.lightInfo.z != 0u);
            int _770 = -_769;
            float _772;
            float _775;
            _772 = 0.0;
            _775 = 0.0;
            float _773;
            float _776;
            for (int _777 = _770; _777 <= _769; _772 = _773, _775 = _776, _777++)
            {
                _773 = _772;
                _776 = _775;
                float _783;
                float _785;
                for (int _786 = _770; _786 <= _769; _773 = _783, _776 = _785, _786++)
                {
                    float4 _802 = shadowAtlas.sample(shadowSampler, ((float2(clamp(_765 + int2(_786, _777), _758, _762)) + float2(0.5)) * Lights.shadowInfo.zw), level(0.0));
                    float _803 = _802.x;
                    float _821;
                    do
                    {
                        if (_668)
                        {
                            float _810 = Shadows.shadowParams[_659].z - Shadows.shadowParams[_659].w;
                            _821 = ((Shadows.shadowParams[_659].z * Shadows.shadowParams[_659].w) / _810) / (_803 + (Shadows.shadowParams[_659].w / _810));
                            break;
                        }
                        _821 = Shadows.shadowParams[_659].z + (_803 * (Shadows.shadowParams[_659].w - Shadows.shadowParams[_659].z));
                        break;
                    } while(false);
                    _785 = _776 + float(_821 >= _744);
                    _783 = _773 + 1.0;
                }
            }
            float _828;
            if (_720)
            {
                _828 = _775 / _772;
            }
            else
            {
                _828 = 1.0;
            }
            _829 = _828;
        }
        else
        {
            _829 = 1.0;
        }
        _443 = _442 + (mix(mix(_543 * (_213.xyz * float3(0.3183098733425140380859375)), _581, float3(0.039999999105930328369140625) + (float3(0.959999978542327880859375) * _482)), (_213.xyz + (_372 * _482)) * _581, _406) * _829);
    }
    float2 _838 = select(in.in_var_TEXCOORD0, in.in_var_TEXCOORD1, bool2(Material.texCoordSets2.x == 1u));
    float4 _853 = emissiveTexture.sample(emissiveSampler, float2(dot(Material.uvTransform[8u].xy, _838) + Material.uvTransform[8u].z, dot(Material.uvTransform[9u].xy, _838) + Material.uvTransform[9u].z));
    float3 _858 = Material.emissiveFactor.xyz * _853.xyz;
    float3 _865;
    if ((in.in_var_DRAWFLAGS & 1u) != 0u)
    {
        _865 = _858 + _213.xyz;
    }
    else
    {
        _865 = _858 + ((mix(mix((_326.xyz * Frame.iblParams.y) * _213.xyz, _347, _396 + (((_396 * _398) * float3(0.085714280605316162109375)) / (float3(1.0) - (float3(0.085714280605316162109375) * _398)))), (_369 + (((_369 * _371) * _375) / (float3(1.0) - (_375 * _371)))) * _347, _406) * (1.0 + (Material.pbrParams.w * (_429.x - 1.0)))) + _442);
    }
    float4 _876;
    if (Material.materialFlags.x == 1u)
    {
        if (_213.w < Material.alphaParams.x)
        {
            discard_fragment();
        }
        float4 _875;
        _875.w = 1.0;
        _876 = _875;
    }
    else
    {
        _876 = _213;
    }
    float3 _883 = _865 * Frame.iblParams.x;
    float3 _942;
    if (Frame.frameFlags.y == 0u)
    {
        float3 _926;
        do
        {
            float _894 = precise::min(_883.x, precise::min(_883.y, _883.z));
            float _902;
            if (_894 < 0.07999999821186065673828125)
            {
                _902 = _894 - ((6.25 * _894) * _894);
            }
            else
            {
                _902 = 0.039999999105930328369140625;
            }
            float3 _904 = _883 - float3(_902);
            float _909 = precise::max(_904.x, precise::max(_904.y, _904.z));
            if (_909 < 0.7599999904632568359375)
            {
                _926 = _904;
                break;
            }
            float _915 = 1.0 - (0.0576000027358531951904296875 / (_909 + (-0.519999980926513671875)));
            _926 = mix(_904 * (_915 / _909), float3(1.0) * _915, float3(1.0 - (1.0 / ((0.1500000059604644775390625 * (_909 - _915)) + 1.0))));
            break;
        } while(false);
        _942 = _926;
    }
    else
    {
        float3 _941;
        if (Frame.frameFlags.y == 1u)
        {
            float3 _930 = float3x3(float3(0.59719002246856689453125, 0.075999997556209564208984375, 0.0284000001847743988037109375), float3(0.354579985141754150390625, 0.908339977264404296875, 0.13382999598979949951171875), float3(0.048229999840259552001953125, 0.0156599991023540496826171875, 0.837769985198974609375)) * _883;
            _941 = fast::clamp(float3x3(float3(1.60475003719329833984375, -0.10208000242710113525390625, -0.00326999998651444911956787109375), float3(-0.5310800075531005859375, 1.108129978179931640625, -0.07276000082492828369140625), float3(-0.0736699998378753662109375, -0.00604999996721744537353515625, 1.0760200023651123046875)) * (((_930 * (_930 + float3(0.02457859925925731658935546875))) - float3(9.0537003416102379560470581054688e-05)) / ((_930 * ((_930 * 0.98372900485992431640625) + float3(0.4329510033130645751953125))) + float3(0.23808099329471588134765625))), float3(0.0), float3(1.0));
        }
        else
        {
            _941 = _883;
        }
        _942 = _941;
    }
    float3 _947;
    if (Frame.frameFlags.x != 0u)
    {
        _947 = powr(_942, float3(0.454545438289642333984375));
    }
    else
    {
        _947 = _942;
    }
    out.out_var_SV_Target0 = float4(_947, _876.w);
    return out;
}

