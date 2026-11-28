// Output transform of glTF scenes (plan F3 Task 5, r.Tonemap / r.Exposure): glTF-Sample-Renderer v1.1.0 (d59d1d6)
// source/Renderer/shaders/tonemapping.glsl toneMap(), ported by value:
//   color *= u_Exposure; operator; linearTosRGB (pow(color, 1 / 2.2), GAMMA = 2.2).
// Operators (Frame.frameFlags.y / Background.flags.y, the r.Tonemap cvar, engine/renderer/Tonemap.cpp):
//   0 = PbrNeutral  toneMap_KhronosPbrNeutral (TONEMAP_KHR_PBR_NEUTRAL, reference mode)
//   1 = Aces        toneMapACES_Hill (TONEMAP_ACES_HILL: ACESInputMat, RRTAndODTFit, ACESOutputMat, clamp)
//   2 = None        no operator (Sample-Renderer's linear / "None" choice: exposure + gamma only)
// As Sample-Renderer, the transform runs in the fragment shader before blending (pbr.frag writes toneMap(color)
// and GL blends that), so BLEND materials mix display values exactly as the reference; a separate full-screen
// tonemap pass would blend in linear HDR instead. The frame-graph post chain (tonemap / bloom / TAA) is F8.
// Included by shaders/pbr/pbr.hlsl and shaders/ibl/background.hlsl (not a pass of its own).
#ifndef POST_TONEMAP_HLSL
#define POST_TONEMAP_HLSL

static const float TONEMAP_GAMMA = 2.2;
static const float TONEMAP_INV_GAMMA = 1.0 / TONEMAP_GAMMA;

float3 linearTosRGB(float3 color)
{
    return pow(color, float3(TONEMAP_INV_GAMMA, TONEMAP_INV_GAMMA, TONEMAP_INV_GAMMA));
}

float3 toneMap_KhronosPbrNeutral(float3 color)
{
    const float startCompression = 0.8 - 0.04;
    const float desaturation = 0.15;
    const float x = min(color.r, min(color.g, color.b));
    const float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;
    const float peak = max(color.r, max(color.g, color.b));
    if (peak < startCompression)
        return color;
    const float d = 1.0 - startCompression;
    const float newPeak = 1.0 - d * d / (peak + d - startCompression);
    color *= newPeak / peak;
    const float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return lerp(color, newPeak * float3(1.0, 1.0, 1.0), g);
}

// GLSL mat3(...) lists columns; an HLSL float3x3(...) lists rows, so GLSL M * v = mul(v, float3x3(same numbers)).
static const float3x3 ACESInputMat = float3x3(0.59719, 0.07600, 0.02840, 0.35458, 0.90834, 0.13383, 0.04823, 0.01566,
                                              0.83777);
static const float3x3 ACESOutputMat = float3x3(1.60475, -0.10208, -0.00327, -0.53108, 1.10813, -0.07276, -0.07367,
                                               -0.00605, 1.07602);

float3 RRTAndODTFit(float3 color)
{
    const float3 a = color * (color + 0.0245786) - 0.000090537;
    const float3 b = color * (0.983729 * color + 0.4329510) + 0.238081;
    return a / b;
}

float3 toneMapACES_Hill(float3 color)
{
    color = mul(color, ACESInputMat);
    color = RRTAndODTFit(color);
    color = mul(color, ACESOutputMat);
    return clamp(color, 0.0, 1.0);
}

// applyGamma = 0 for an sRGB target (the hardware encodes); Sample-Renderer always applies linearTosRGB (UNORM canvas).
float3 toneMapOutput(float3 color, float exposure, uint applyGamma, uint op)
{
    color *= exposure;
    if (op == 0u)
    {
        color = toneMap_KhronosPbrNeutral(color);
    }
    else if (op == 1u)
    {
        color = toneMapACES_Hill(color);
    }
    if (applyGamma != 0u)
    {
        color = linearTosRGB(color);
    }
    return color;
}

#endif // POST_TONEMAP_HLSL
