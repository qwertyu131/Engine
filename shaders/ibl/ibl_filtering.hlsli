// IBL filtering of glTF-Sample-Renderer v1.1.0 (d59d1d6), ported by value (plan F3 Task 5, spec K-B reference mode):
//   source/shaders/ibl_filtering.frag (uvToXYZ, dirToUV, radicalInverse_VdC, hammersley2d, generateTBN, D_GGX, GGX,
//   Lambertian, getImportanceSample, computeLod, filterColor, V_SmithGGXCorrelated, LUT)
//   source/shaders/fullscreen.vert (one triangle; texCoord = the pixel centre / target size)
// The C++ twin is engine/renderer/IblCpu.cpp (same functions, same names); the two must stay equal by value.
//
// Face orientation: the filtered cubes have the memory layout Sample-Renderer writes. Its fragment at window row y of
// face `f` gets texCoord (x + 0.5, y + 0.5) / size, and GL / D3D / Vulkan all map a cube direction to the face texel
// (memory row y) through the same s/t table, so a literal port (texCoord = SV_Position.xy / size) reproduces it.
// Only u_distribution GGX (1) and Lambertian (0) are ported (Charlie / sheen is F6).
#ifndef IBL_FILTERING_HLSLI
#define IBL_FILTERING_HLSLI

static const float MATH_PI = 3.1415926535897932384626433832795;
static const int cLambertian = 0;
static const int cGGX = 1;

cbuffer Filter : register(b0, space0)
{
    int4 filterInts;     // x = u_currentFace, y = u_sampleCount, z = u_width, w = u_distribution
    float4 filterFloats; // x = u_roughness, y = u_lodBias, z = u_intensityScale, w = target size (this mip's edge)
};

struct FullscreenOut
{
    float4 position : SV_Position;
};

// fullscreen.vert: x = (id & 1) << 2, y = (id & 2) << 1 -> (0, 0), (4, 0), (0, 4) - 1.
FullscreenOut VSMain(uint vertexId : SV_VertexID)
{
    const float x = float((vertexId & 1u) << 2);
    const float y = float((vertexId & 2u) << 1);
    FullscreenOut o;
    o.position = float4(x - 1.0, y - 1.0, 0.0, 1.0);
    return o;
}

float3 uvToXYZ(int face, float2 uv)
{
    if (face == 0)
        return float3(1.0, uv.y, -uv.x);
    else if (face == 1)
        return float3(-1.0, uv.y, uv.x);
    else if (face == 2)
        return float3(+uv.x, -1.0, +uv.y);
    else if (face == 3)
        return float3(+uv.x, 1.0, -uv.y);
    else if (face == 4)
        return float3(+uv.x, uv.y, 1.0);
    return float3(-uv.x, +uv.y, -1.0);
}

float2 dirToUV(float3 dir)
{
    // GLSL atan(y, x) = HLSL atan2(y, x).
    return float2(0.5 + 0.5 * atan2(dir.z, dir.x) / MATH_PI, 1.0 - acos(dir.y) / MATH_PI);
}

float saturate1(float v)
{
    return clamp(v, 0.0, 1.0);
}

float radicalInverse_VdC(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10; // / 0x100000000
}

float2 hammersley2d(int i, int N)
{
    return float2(float(i) / float(N), radicalInverse_VdC(uint(i)));
}

// Columns tangent, bitangent, normal (GLSL mat3(tangent, bitangent, normal)); returned as the three vectors.
void generateTBN(float3 normal, out float3 tangent, out float3 bitangent)
{
    bitangent = float3(0.0, 1.0, 0.0);
    const float NdotUp = dot(normal, float3(0.0, 1.0, 0.0));
    const float epsilon = 0.0000001;
    if (1.0 - abs(NdotUp) <= epsilon)
    {
        // Sampling +Y or -Y, so we need a more robust bitangent.
        if (NdotUp > 0.0)
            bitangent = float3(0.0, 0.0, 1.0);
        else
            bitangent = float3(0.0, 0.0, -1.0);
    }
    tangent = normalize(cross(bitangent, normal));
    bitangent = cross(normal, tangent);
}

struct MicrofacetDistributionSample
{
    float pdf;
    float cosTheta;
    float sinTheta;
    float phi;
};

float D_GGX_filter(float NdotH, float roughness)
{
    const float a = NdotH * roughness;
    const float k = roughness / (1.0 - NdotH * NdotH + a * a);
    return k * k * (1.0 / MATH_PI);
}

MicrofacetDistributionSample GGX(float2 xi, float roughness)
{
    MicrofacetDistributionSample ggx;
    const float alpha = roughness * roughness;
    ggx.cosTheta = saturate1(sqrt((1.0 - xi.y) / (1.0 + (alpha * alpha - 1.0) * xi.y)));
    ggx.sinTheta = sqrt(1.0 - ggx.cosTheta * ggx.cosTheta);
    ggx.phi = 2.0 * MATH_PI * xi.x;
    ggx.pdf = D_GGX_filter(ggx.cosTheta, alpha);
    ggx.pdf /= 4.0;
    return ggx;
}

MicrofacetDistributionSample Lambertian(float2 xi, float roughness)
{
    MicrofacetDistributionSample lambertian;
    lambertian.cosTheta = sqrt(1.0 - xi.y);
    lambertian.sinTheta = sqrt(xi.y);
    lambertian.phi = 2.0 * MATH_PI * xi.x;
    lambertian.pdf = lambertian.cosTheta / MATH_PI;
    return lambertian;
}

float4 getImportanceSample(int sampleIndex, float3 N, float roughness, int sampleCount, int distribution)
{
    const float2 xi = hammersley2d(sampleIndex, sampleCount);
    MicrofacetDistributionSample importanceSample;
    if (distribution == cLambertian)
        importanceSample = Lambertian(xi, roughness);
    else
        importanceSample = GGX(xi, roughness);
    const float3 localSpaceDirection =
        normalize(float3(importanceSample.sinTheta * cos(importanceSample.phi),
                         importanceSample.sinTheta * sin(importanceSample.phi), importanceSample.cosTheta));
    float3 tangent, bitangent;
    generateTBN(N, tangent, bitangent);
    const float3 direction =
        tangent * localSpaceDirection.x + bitangent * localSpaceDirection.y + N * localSpaceDirection.z;
    return float4(direction, importanceSample.pdf);
}

float computeLod(float pdf, int width, int sampleCount)
{
    return 0.5 * log2(6.0 * float(width) * float(width) / (float(sampleCount) * pdf));
}

float V_SmithGGXCorrelated(float NoV, float NoL, float roughness)
{
    const float a2 = pow(roughness, 4.0);
    const float GGXV = NoL * sqrt(NoV * NoV * (1.0 - a2) + a2);
    const float GGXL = NoV * sqrt(NoL * NoL * (1.0 - a2) + a2);
    return 0.5 / (GGXV + GGXL);
}

// --- cube filtering (prefilter_ggx.hlsl, prefilter_lambert.hlsl) ------------------------------------------------
#ifdef IBL_FILTER_CUBE
TextureCube<float4> uCubeMap : register(t1, space0); // the source cube with its box mip chain
SamplerState uCubeMapSampler : register(s2, space0); // LINEAR_MIPMAP_LINEAR, clamp to edge

float3 filterColor(float3 N, int distribution)
{
    const int sampleCount = filterInts.y;
    const int width = filterInts.z;
    const float roughness = filterFloats.x;
    const float lodBias = filterFloats.y;
    const float intensityScale = filterFloats.z;
    float3 color = float3(0.0, 0.0, 0.0);
    float weight = 0.0;
    for (int i = 0; i < sampleCount; ++i)
    {
        const float4 importanceSample = getImportanceSample(i, N, roughness, sampleCount, distribution);
        const float3 H = importanceSample.xyz;
        const float pdf = importanceSample.w;
        float lod = computeLod(pdf, width, sampleCount);
        lod += lodBias;
        if (distribution == cLambertian)
        {
            const float3 lambertian = uCubeMap.SampleLevel(uCubeMapSampler, H, lod).rgb * intensityScale;
            color += lambertian;
        }
        else
        {
            const float3 V = N;
            const float3 L = normalize(reflect(-V, H));
            const float NdotL = dot(N, L);
            if (NdotL > 0.0)
            {
                if (roughness == 0.0)
                {
                    // without this the roughness = 0 distribution gets blurry
                    lod = lodBias;
                }
                const float3 sampleColor = uCubeMap.SampleLevel(uCubeMapSampler, L, lod).rgb * intensityScale;
                color += sampleColor * NdotL;
                weight += NdotL;
            }
        }
    }
    if (weight != 0.0)
        color /= weight;
    else
        color /= float(sampleCount);
    return color;
}

// main() of ibl_filtering.frag with u_isGeneratingLUT = 0 and u_floatTexture = 1 (the FLOAT path: the reference
// browser has EXT_color_buffer_float + OES_texture_float_linear, checked with Firefox 160 on this machine).
float4 filterMain(float4 position, int distribution)
{
    const float2 texCoord = position.xy / filterFloats.w;
    const float2 newUV = texCoord * 2.0 - 1.0;
    const float3 scan = uvToXYZ(filterInts.x, newUV);
    float3 direction = normalize(scan);
    direction.y = -direction.y;
    return float4(filterColor(direction, distribution), 1.0);
}
#endif

#endif // IBL_FILTERING_HLSLI
