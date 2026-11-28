// Punctual lights (KHR_lights_punctual) and their shadows (plan F3 Task 7).
//
// Ported by value from glTF-Sample-Renderer v1.1.0 (d59d1d6, the @khronosgroup/gltf-viewer 1.1.0 bundle):
//   punctual.glsl  getRangeAttenuation, getSpotAttenuation, getLighIntensity (sic), struct Light;
//   pbr.frag       the USE_PUNCTUAL loop (dielectric / metal Fresnel with abs(VdotH), l_diffuse, l_specular_*,
//                  l_dielectric_brdf = mix(l_diffuse, l_specular_dielectric, dielectric_fresnel), l_metal_brdf,
//                  l_color = mix(l_dielectric_brdf, l_metal_brdf, metallic), color += l_color), without the F6 material
//                  extensions (transmission, diffuse transmission, anisotropy, iridescence, clearcoat, sheen);
//   brdf.glsl      F_Schlick, V_GGX, D_GGX, BRDF_lambertian, BRDF_specularGGX (shaders/common/brdf.hlsli, F1).
// Light values come from light.js toUniform (direction = node rotation of (0, 0, -1), position = node translation, range
// -1 or the glTF range, cone cosines) through engine/renderer/Lights.cpp. No unit conversion (Sample-Renderer uses the
// glTF intensity as is). Occlusion applies to the IBL term only (pbr.frag), so it is applied before this loop.
//
// Additions of this renderer (normal mode only; Sample-Viewer has neither, so reference mode turns both off):
//   r.Lighting.Specular = 0 (lightInfo.y = 0): l_specular_* = 0;
//   shadows: lights[4i + 3].z >= 0 names the light's first shadow-atlas tile: directional = cascades picked by view
//   depth (shadowRects.w), spot = one tile, point = six cube-face tiles picked by the major axis of light -> point
//   (+X, -X, +Y, -Y, +Z, -Z). The lookup offsets the point along the geometric normal (shadowInfo.x texels), projects
//   it, and compares linear depths (the tile's near / far) with a bias of shadowInfo.y texels; PCF = 3 x 3 texel-centre
//   nearest samples clamped to the tile.
#ifndef LIGHTS_HLSLI
#define LIGHTS_HLSLI

#include "../common/bindings.hlsli"
#include "../common/brdf.hlsli"

// punctual.glsl getRangeAttenuation
float getRangeAttenuation(float range, float distance)
{
    if (range <= 0.0)
    {
        // negative range means unlimited
        return 1.0 / pow(distance, 2.0);
    }
    return max(min(1.0 - pow(distance / range, 4.0), 1.0), 0.0) / pow(distance, 2.0);
}

// punctual.glsl getSpotAttenuation
float getSpotAttenuation(float3 pointToLight, float3 spotDirection, float outerConeCos, float innerConeCos)
{
    const float actualCos = dot(normalize(spotDirection), normalize(-pointToLight));
    if (actualCos > outerConeCos)
    {
        if (actualCos < innerConeCos)
        {
            const float angularAttenuation = (actualCos - outerConeCos) / (innerConeCos - outerConeCos);
            return angularAttenuation * angularAttenuation;
        }
        return 1.0;
    }
    return 0.0;
}

// punctual.glsl getLighIntensity
float3 getLightIntensity(uint i, float3 pointToLight)
{
    const float4 l0 = lights[4u * i];
    const float4 l1 = lights[4u * i + 1u];
    const float4 l2 = lights[4u * i + 2u];
    const float4 l3 = lights[4u * i + 3u];
    const uint type = uint(l3.y);
    float rangeAttenuation = 1.0;
    float spotAttenuation = 1.0;
    if (type != LIGHT_DIRECTIONAL)
    {
        rangeAttenuation = getRangeAttenuation(l0.w, length(pointToLight));
    }
    if (type == LIGHT_SPOT)
    {
        spotAttenuation = getSpotAttenuation(pointToLight, l0.xyz, l3.x, l2.w);
    }
    return rangeAttenuation * spotAttenuation * l1.w * l1.rgb;
}

// Linear depth (distance along the tile's view axis) of a stored / projected clip depth z of tile `tile`.
float shadowLinearDepth(uint tile, float z)
{
    const float4 p = shadowParams[tile];
    if (p.y > 0.5)
    {
        // perspective, right-handed, clip z in [0, 1]: z = -A + B / d with A = f / (n - f), B = n f / (n - f)
        const float a = p.w / (p.z - p.w);
        const float b = p.z * p.w / (p.z - p.w);
        return b / (z + a);
    }
    return p.z + z * (p.w - p.z); // orthographic
}

// Visibility (0 shadowed .. 1 lit) of world point `worldPos` (geometric normal `ng`) in shadow tile `tile`.
float shadowTileVisibility(uint tile, float3 worldPos, float3 ng, float3 lightPos)
{
    const float4 params = shadowParams[tile];
    const float4 rect = shadowRects[tile];
    const float texelWorld = params.y > 0.5 ? params.x * max(length(worldPos - lightPos), 1e-4) : params.x;
    const float3 p = worldPos + ng * (shadowInfo.x * texelWorld);
    const float4 clip = mul(shadowMatrices[tile], float4(p, 1.0));
    const float3 ndc = clip.xyz / max(abs(clip.w), 1e-20);
    // Outside the tile's volume (or behind a perspective tile's eye): no caster was rendered there, lit.
    const bool inside = clip.w > 0.0 && ndc.z >= 0.0 && ndc.z <= 1.0 && abs(ndc.x) <= 1.0 && abs(ndc.y) <= 1.0;
    const float reference = shadowLinearDepth(tile, saturate(ndc.z)) - shadowInfo.y * texelWorld;
    // Texture UV (0, 0) = the first row (RHI convention): v = 0.5 - 0.5 ndc.y.
    const float2 texel = rect.xy + float2(saturate(ndc.x * 0.5 + 0.5), saturate(0.5 - ndc.y * 0.5)) * rect.z;
    const int2 lo = int2(rect.xy);
    const int2 hi = int2(rect.xy) + int2(rect.zz) - int2(1, 1);
    const int2 centre = clamp(int2(floor(texel)), lo, hi);
    const int radius = lightInfo.z != 0u ? 1 : 0;
    float lit = 0.0;
    float taps = 0.0;
    for (int y = -radius; y <= radius; ++y)
    {
        for (int x = -radius; x <= radius; ++x)
        {
            const int2 t = clamp(centre + int2(x, y), lo, hi);
            const float2 uv = (float2(t) + 0.5) * shadowInfo.zw;
            const float stored = shadowLinearDepth(tile, shadowAtlas.SampleLevel(shadowSampler, uv, 0.0));
            lit += stored >= reference ? 1.0 : 0.0;
            taps += 1.0;
        }
    }
    return inside ? lit / taps : 1.0;
}

// Visibility of light i at world point `worldPos` (1 without a shadow). Single exit, every variable initialised: the
// dxbc cross path (SPIR-V -> HLSL -> FXC) rejects values that are only assigned on some paths.
float lightVisibility(uint i, float3 worldPos, float3 ng)
{
    const float4 l3 = lights[4u * i + 3u];
    const float3 lightPos = lights[4u * i + 2u].xyz;
    const uint type = uint(l3.y);
    const uint first = uint(max(l3.z, 0.0));
    const uint count = uint(max(l3.w, 1.0));
    bool shadowed = l3.z >= 0.0;
    uint tile = first;
    if (type == LIGHT_DIRECTIONAL)
    {
        // The first cascade whose far view depth covers the point; none beyond r.Shadow.Distance.
        const float depth = dot(worldPos - cameraPosition.xyz, viewForward.xyz);
        bool found = false;
        for (uint c = 0u; c < count; ++c)
        {
            const bool here = !found && depth <= shadowRects[first + c].w;
            tile = here ? first + c : tile;
            found = found || here;
        }
        shadowed = shadowed && found;
    }
    else if (type == LIGHT_POINT)
    {
        // Cube face of the major axis of light -> point: +X, -X, +Y, -Y, +Z, -Z.
        const float3 d = worldPos - lightPos;
        const float3 a = abs(d);
        const uint faceX = d.x >= 0.0 ? 0u : 1u;
        const uint faceY = d.y >= 0.0 ? 2u : 3u;
        const uint faceZ = d.z >= 0.0 ? 4u : 5u;
        const uint face = (a.x >= a.y && a.x >= a.z) ? faceX : (a.y >= a.z ? faceY : faceZ);
        tile = first + min(face, count - 1u);
    }
    float visibility = 1.0;
    if (shadowed)
    {
        visibility = shadowTileVisibility(tile, worldPos, ng, lightPos);
    }
    return visibility;
}

// pbr.frag USE_PUNCTUAL loop: the sum of l_color over the lights of this group.
float3 punctualLighting(float3 worldPos, float3 n, float3 ng, float3 v, float3 baseColor, float metallic,
                        float alphaRoughness, float3 f0Dielectric, float specularWeight)
{
    float3 color = float3(0.0, 0.0, 0.0);
    const float3 f90Dielectric = float3(1.0, 1.0, 1.0);
    const uint count = min(lightInfo.x, uint(MAX_LIGHTS));
    for (uint i = 0u; i < count; ++i)
    {
        const float4 l0 = lights[4u * i];
        const float4 l2 = lights[4u * i + 2u];
        const uint type = uint(lights[4u * i + 3u].y);
        // pbr.frag: position - v_Position for point / spot, -direction for directional.
        const float3 pointToLight = type != LIGHT_DIRECTIONAL ? l2.xyz - worldPos : -l0.xyz;
        const float3 l = normalize(pointToLight);
        const float3 h = normalize(l + v);
        const float NdotL = clampedDot(n, l);
        const float NdotV = clampedDot(n, v);
        const float NdotH = clampedDot(n, h);
        const float VdotH = clampedDot(v, h);
        const float3 dielectric_fresnel = F_Schlick(f0Dielectric * specularWeight, f90Dielectric, abs(VdotH));
        const float3 metal_fresnel = F_Schlick(baseColor, float3(1.0, 1.0, 1.0), abs(VdotH));
        const float3 lightIntensity = getLightIntensity(i, pointToLight);
        const float3 l_diffuse = lightIntensity * NdotL * BRDF_lambertian(baseColor);
        float3 l_specular_metal = float3(0.0, 0.0, 0.0);
        if (lightInfo.y != 0u)
        {
            l_specular_metal = lightIntensity * NdotL * BRDF_specularGGX(alphaRoughness, NdotL, NdotV, NdotH);
        }
        const float3 l_specular_dielectric = l_specular_metal;
        const float3 l_metal_brdf = metal_fresnel * l_specular_metal;
        const float3 l_dielectric_brdf = lerp(l_diffuse, l_specular_dielectric, dielectric_fresnel);
        const float3 l_color = lerp(l_dielectric_brdf, l_metal_brdf, metallic);
        color += l_color * lightVisibility(i, worldPos, ng);
    }
    return color;
}

#endif // LIGHTS_HLSLI
