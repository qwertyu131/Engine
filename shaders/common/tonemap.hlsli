// Output transform, as Sample-Viewer toneMap() with TONEMAP_KHR_PBR_NEUTRAL (spec K-B reference mode):
// exposure -> Khronos PBR Neutral -> linearTosRGB (gamma 2.2, INV_GAMMA = 1 / 2.2).
#ifndef TONEMAP_HLSLI
#define TONEMAP_HLSLI

float3 toneMapKhronosPbrNeutral(float3 color)
{
    const float startCompression = 0.8 - 0.04;
    const float desaturation = 0.15;
    const float x = min(color.r, min(color.g, color.b));
    const float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;
    const float peak = max(color.r, max(color.g, color.b));
    if (peak < startCompression)
    {
        return color;
    }
    const float d = 1.0 - startCompression;
    const float newPeak = 1.0 - d * d / (peak + d - startCompression);
    color *= newPeak / peak;
    const float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return lerp(color, newPeak * float3(1.0, 1.0, 1.0), g);
}

float3 linearToSrgbGamma22(float3 color)
{
    return pow(color, float3(1.0 / 2.2, 1.0 / 2.2, 1.0 / 2.2));
}

// exposure: Frame.iblParams.x; applyGamma: Frame.frameFlags.x.
float3 toneMap(float3 color, float exposure, uint applyGamma)
{
    color = toneMapKhronosPbrNeutral(color * exposure);
    if (applyGamma != 0u)
    {
        color = linearToSrgbGamma22(color);
    }
    return color;
}

#endif // TONEMAP_HLSLI
