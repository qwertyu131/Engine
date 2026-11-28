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
    float3 _436 = mix(mix((irradianceEnvTexture.sample(envSampler, (_280 * _323)).xyz * Frame.iblParams.y) * _213.xyz, _347, _396 + (((_396 * _398) * float3(0.085714280605316162109375)) / (float3(1.0) - (float3(0.085714280605316162109375) * _398)))), (_369 + (((_369 * _371) * _375) / (float3(1.0) - (_375 * _371)))) * _347, _406) * (1.0 + (Material.pbrParams.w * (_429.x - 1.0)));
    float _437 = _312 * _312;
    float3 _835;
    if (Lights.lightInfo.x != 0u)
    {
        uint _443 = min(Lights.lightInfo.x, 64u);
        float3 _445;
        _445 = float3(0.0);
        float3 _446;
        for (uint _448 = 0u; _448 < _443; _445 = _446, _448++)
        {
            uint _453 = 4u * _448;
            uint _456 = _453 + 2u;
            uint _459 = _453 + 3u;
            float3 _472;
            if (uint(Lights.lights[_459].y) != 0u)
            {
                _472 = Lights.lights[_456].xyz - in.in_var_WORLD_POSITION;
            }
            else
            {
                _472 = -Lights.lights[_453].xyz;
            }
            float3 _473 = fast::normalize(_472);
            float3 _475 = fast::normalize(_473 + _218);
            float _477 = fast::clamp(dot(_280, _473), 0.0, 1.0);
            float _479 = fast::clamp(dot(_280, _475), 0.0, 1.0);
            float _485 = powr(fast::clamp(1.0 - abs(fast::clamp(dot(_218, _475), 0.0, 1.0)), 0.0, 1.0), 5.0);
            uint _490 = _453 + 1u;
            uint _495 = uint(Lights.lights[_459].y);
            float _516;
            if (_495 != 0u)
            {
                float _500 = length(_472);
                float _515;
                do
                {
                    if (Lights.lights[_453].w <= 0.0)
                    {
                        _515 = 1.0 / powr(_500, 2.0);
                        break;
                    }
                    _515 = precise::max(precise::min(1.0 - powr(_500 / Lights.lights[_453].w, 4.0), 1.0), 0.0) / powr(_500, 2.0);
                    break;
                } while(false);
                _516 = _515;
            }
            else
            {
                _516 = 1.0;
            }
            float _540;
            if (_495 == 2u)
            {
                float _539;
                do
                {
                    float _528 = dot(fast::normalize(Lights.lights[_453].xyz), fast::normalize(-_472));
                    if (_528 > Lights.lights[_459].x)
                    {
                        if (_528 < Lights.lights[_456].w)
                        {
                            float _537 = (_528 - Lights.lights[_459].x) / (Lights.lights[_456].w - Lights.lights[_459].x);
                            _539 = _537 * _537;
                            break;
                        }
                        _539 = 1.0;
                        break;
                    }
                    _539 = 0.0;
                    break;
                } while(false);
                _540 = _539;
            }
            else
            {
                _540 = 1.0;
            }
            float3 _546 = (Lights.lights[_490].xyz * ((_516 * _540) * Lights.lights[_490].w)) * _477;
            float3 _584;
            if (Lights.lightInfo.y != 0u)
            {
                float _556;
                float _573;
                do
                {
                    _556 = _437 * _437;
                    float _558 = 1.0 - _556;
                    float _568 = (_477 * sqrt(((_349 * _349) * _558) + _556)) + (_349 * sqrt(((_477 * _477) * _558) + _556));
                    if (_568 > 0.0)
                    {
                        _573 = 0.5 / _568;
                        break;
                    }
                    _573 = 0.0;
                    break;
                } while(false);
                float _577 = ((_479 * _479) * (_556 - 1.0)) + 1.0;
                _584 = _546 * float3(_573 * (_556 / ((3.1415927410125732421875 * _577) * _577)));
            }
            else
            {
                _584 = float3(0.0);
            }
            uint _591 = uint(precise::max(Lights.lights[_459].z, 0.0));
            uint _594 = uint(precise::max(Lights.lights[_459].w, 1.0));
            bool _595 = Lights.lights[_459].z >= 0.0;
            uint _662;
            bool _663;
            if (_495 == 0u)
            {
                float _604 = dot(in.in_var_WORLD_POSITION - Frame.cameraPosition.xyz, Lights.viewForward.xyz);
                uint _606;
                bool _609;
                _606 = _591;
                _609 = false;
                bool _610;
                uint _607;
                for (uint _611 = 0u; _611 < _594; _606 = _607, _609 = _610, _611++)
                {
                    bool _616 = !_609;
                    bool _623;
                    if (_616)
                    {
                        _623 = _604 <= Shadows.shadowRects[_591 + _611].w;
                    }
                    else
                    {
                        _623 = false;
                    }
                    if (_623)
                    {
                        _607 = _591 + _611;
                    }
                    else
                    {
                        _607 = _606;
                    }
                    _610 = _616 ? _623 : true;
                }
                _662 = _606;
                _663 = _595 ? _609 : false;
            }
            else
            {
                uint _661;
                if (_495 == 1u)
                {
                    float3 _632 = in.in_var_WORLD_POSITION - Lights.lights[_456].xyz;
                    float3 _633 = abs(_632);
                    float _643 = _633.x;
                    float _644 = _633.y;
                    bool _650;
                    if (_643 >= _644)
                    {
                        _650 = _643 >= _633.z;
                    }
                    else
                    {
                        _650 = false;
                    }
                    uint _657;
                    if (_650)
                    {
                        _657 = (_632.x >= 0.0) ? 0u : 1u;
                    }
                    else
                    {
                        _657 = (_644 >= _633.z) ? ((_632.y >= 0.0) ? 2u : 3u) : ((_632.z >= 0.0) ? 4u : 5u);
                    }
                    _661 = _591 + min(_657, (_594 - 1u));
                }
                else
                {
                    _661 = _591;
                }
                _662 = _661;
                _663 = _595;
            }
            float _832;
            if (_663)
            {
                bool _671 = Shadows.shadowParams[_662].y > 0.5;
                float _681;
                if (_671)
                {
                    _681 = Shadows.shadowParams[_662].x * precise::max(length(in.in_var_WORLD_POSITION - Lights.lights[_456].xyz), 9.9999997473787516355514526367188e-05);
                }
                else
                {
                    _681 = Shadows.shadowParams[_662].x;
                }
                float4 _694 = Shadows.shadowMatrices[_662] * float4(in.in_var_WORLD_POSITION + (_230 * (Lights.shadowInfo.x * _681)), 1.0);
                float _696 = _694.w;
                float3 _700 = _694.xyz / float3(precise::max(abs(_696), 9.9999996826552253889678874634872e-21));
                bool _706;
                if (_696 > 0.0)
                {
                    _706 = _700.z >= 0.0;
                }
                else
                {
                    _706 = false;
                }
                bool _711;
                if (_706)
                {
                    _711 = _700.z <= 1.0;
                }
                else
                {
                    _711 = false;
                }
                bool _717;
                if (_711)
                {
                    _717 = abs(_700.x) <= 1.0;
                }
                else
                {
                    _717 = false;
                }
                bool _723;
                if (_717)
                {
                    _723 = abs(_700.y) <= 1.0;
                }
                else
                {
                    _723 = false;
                }
                float _725 = fast::clamp(_700.z, 0.0, 1.0);
                float _743;
                do
                {
                    if (_671)
                    {
                        float _732 = Shadows.shadowParams[_662].z - Shadows.shadowParams[_662].w;
                        _743 = ((Shadows.shadowParams[_662].z * Shadows.shadowParams[_662].w) / _732) / (_725 + (Shadows.shadowParams[_662].w / _732));
                        break;
                    }
                    _743 = Shadows.shadowParams[_662].z + (_725 * (Shadows.shadowParams[_662].w - Shadows.shadowParams[_662].z));
                    break;
                } while(false);
                float _747 = _743 - (Lights.shadowInfo.y * _681);
                int2 _761 = int2(Shadows.shadowRects[_662].xy);
                int2 _765 = (_761 + int2(Shadows.shadowRects[_662].zz)) - int2(1);
                int2 _768 = clamp(int2(floor(Shadows.shadowRects[_662].xy + (float2(fast::clamp((_700.x * 0.5) + 0.5, 0.0, 1.0), fast::clamp(0.5 - (_700.y * 0.5), 0.0, 1.0)) * Shadows.shadowRects[_662].z))), _761, _765);
                int _772 = int(Lights.lightInfo.z != 0u);
                int _773 = -_772;
                float _775;
                float _778;
                _775 = 0.0;
                _778 = 0.0;
                float _776;
                float _779;
                for (int _780 = _773; _780 <= _772; _775 = _776, _778 = _779, _780++)
                {
                    _776 = _775;
                    _779 = _778;
                    float _786;
                    float _788;
                    for (int _789 = _773; _789 <= _772; _776 = _786, _779 = _788, _789++)
                    {
                        float4 _805 = shadowAtlas.sample(shadowSampler, ((float2(clamp(_768 + int2(_789, _780), _761, _765)) + float2(0.5)) * Lights.shadowInfo.zw), level(0.0));
                        float _806 = _805.x;
                        float _824;
                        do
                        {
                            if (_671)
                            {
                                float _813 = Shadows.shadowParams[_662].z - Shadows.shadowParams[_662].w;
                                _824 = ((Shadows.shadowParams[_662].z * Shadows.shadowParams[_662].w) / _813) / (_806 + (Shadows.shadowParams[_662].w / _813));
                                break;
                            }
                            _824 = Shadows.shadowParams[_662].z + (_806 * (Shadows.shadowParams[_662].w - Shadows.shadowParams[_662].z));
                            break;
                        } while(false);
                        _788 = _779 + float(_824 >= _747);
                        _786 = _776 + 1.0;
                    }
                }
                float _831;
                if (_723)
                {
                    _831 = _778 / _775;
                }
                else
                {
                    _831 = 1.0;
                }
                _832 = _831;
            }
            else
            {
                _832 = 1.0;
            }
            _446 = _445 + (mix(mix(_546 * (_213.xyz * float3(0.3183098733425140380859375)), _584, float3(0.039999999105930328369140625) + (float3(0.959999978542327880859375) * _485)), (_213.xyz + (_372 * _485)) * _584, _406) * _832);
        }
        _835 = _436 + _445;
    }
    else
    {
        _835 = _436;
    }
    float2 _842 = select(in.in_var_TEXCOORD0, in.in_var_TEXCOORD1, bool2(Material.texCoordSets2.x == 1u));
    float4 _857 = emissiveTexture.sample(emissiveSampler, float2(dot(Material.uvTransform[8u].xy, _842) + Material.uvTransform[8u].z, dot(Material.uvTransform[9u].xy, _842) + Material.uvTransform[9u].z));
    float3 _862 = Material.emissiveFactor.xyz * _857.xyz;
    float3 _869;
    if ((in.in_var_DRAWFLAGS & 1u) != 0u)
    {
        _869 = _862 + _213.xyz;
    }
    else
    {
        _869 = _862 + _835;
    }
    float4 _880;
    if (Material.materialFlags.x == 1u)
    {
        if (_213.w < Material.alphaParams.x)
        {
            discard_fragment();
        }
        float4 _879;
        _879.w = 1.0;
        _880 = _879;
    }
    else
    {
        _880 = _213;
    }
    float3 _887 = _869 * Frame.iblParams.x;
    float3 _946;
    if (Frame.frameFlags.y == 0u)
    {
        float3 _930;
        do
        {
            float _898 = precise::min(_887.x, precise::min(_887.y, _887.z));
            float _906;
            if (_898 < 0.07999999821186065673828125)
            {
                _906 = _898 - ((6.25 * _898) * _898);
            }
            else
            {
                _906 = 0.039999999105930328369140625;
            }
            float3 _908 = _887 - float3(_906);
            float _913 = precise::max(_908.x, precise::max(_908.y, _908.z));
            if (_913 < 0.7599999904632568359375)
            {
                _930 = _908;
                break;
            }
            float _919 = 1.0 - (0.0576000027358531951904296875 / (_913 + (-0.519999980926513671875)));
            _930 = mix(_908 * (_919 / _913), float3(1.0) * _919, float3(1.0 - (1.0 / ((0.1500000059604644775390625 * (_913 - _919)) + 1.0))));
            break;
        } while(false);
        _946 = _930;
    }
    else
    {
        float3 _945;
        if (Frame.frameFlags.y == 1u)
        {
            float3 _934 = float3x3(float3(0.59719002246856689453125, 0.075999997556209564208984375, 0.0284000001847743988037109375), float3(0.354579985141754150390625, 0.908339977264404296875, 0.13382999598979949951171875), float3(0.048229999840259552001953125, 0.0156599991023540496826171875, 0.837769985198974609375)) * _887;
            _945 = fast::clamp(float3x3(float3(1.60475003719329833984375, -0.10208000242710113525390625, -0.00326999998651444911956787109375), float3(-0.5310800075531005859375, 1.108129978179931640625, -0.07276000082492828369140625), float3(-0.0736699998378753662109375, -0.00604999996721744537353515625, 1.0760200023651123046875)) * (((_934 * (_934 + float3(0.02457859925925731658935546875))) - float3(9.0537003416102379560470581054688e-05)) / ((_934 * ((_934 * 0.98372900485992431640625) + float3(0.4329510033130645751953125))) + float3(0.23808099329471588134765625))), float3(0.0), float3(1.0));
        }
        else
        {
            _945 = _887;
        }
        _946 = _945;
    }
    float3 _951;
    if (Frame.frameFlags.x != 0u)
    {
        _951 = powr(_946, float3(0.454545438289642333984375));
    }
    else
    {
        _951 = _946;
    }
    out.out_var_SV_Target0 = float4(_951, _880.w);
    return out;
}

