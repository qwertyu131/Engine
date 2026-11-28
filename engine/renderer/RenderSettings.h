#pragma once
// Renderer cvars of the glTF passes (plan F3 Task 5, spec §8 / K-B / K-F) and reference mode.
//
//   r.ReferenceMode       bool    0     1 = reproduce tests/golden/ref/sample-viewer/settings.json (spec K-B)
//   r.IBL.Environment     string  Cannon_Exterior   a name (<assetDir>/environments/<name>.hdr), a .hdr path, or
//                                       "builtin" (the deterministic sky / ground gradient of builtin:pbr-spheres)
//   r.IBL.Rotation        float   90    environment rotation about +Y in degrees (Sample-Viewer environmentRotation)
//   r.IBL.Intensity       float   1     u_EnvIntensity (iblIntensity x iblIntensityScale, the scale is 1: FLOAT path)
//   r.IBL.Resolution      int     256   iblSampler.textureSize (K-F); a power of two
//   r.IBL.SpecularSamples int     1024  iblSampler.ggxSampleCount (K-F)
//   r.IBL.DiffuseSamples  int     2048  iblSampler.lambertianSampleCount
//   r.Exposure            float   1     u_Exposure
//   r.Tonemap             string  PbrNeutral   PbrNeutral | Aces | None (shaders/post/tonemap.hlsl)
//   r.Background          string  Environment  Environment (the blurred environment) | Color (r.Background.Color)
//   r.Background.Color    string  0,0,0        display colour r,g,b in [0, 1] for r.Background=Color
//   r.Background.Blur     float   0.6   u_EnvBlurNormalized (Sample-Viewer blurEnvironmentMap: 0.6 on, 0 off)
//
// Lighting and shadows (plan F3 Task 7, spec K-F; the sg.* table sets most of them, Config/DefaultScalability.ini):
//   r.ReferenceMode.Punctual bool  0     reference mode draws KHR_lights_punctual only when 1 (the settings.json
//                                        usePunctual of the references it is compared with)
//   r.Lighting.Punctual   bool    1     normal mode: draw the scene's punctual lights
//   r.Lighting.MaxLights  int     64    lights per object (forward light loop); T0 caps it at 8, T1 / T2 at 64
//   r.Lighting.Specular   bool    1     punctual specular lobe (0: diffuse only)
//   r.Shadow.Enable       bool    1     normal-mode shadows (reference mode never draws shadows: Sample-Viewer has none)
//   r.Shadow.CascadeCount int     3     directional light cascades, 1..4
//   r.Shadow.MapResolution int    2048  shadow map tile edge (texels)
//   r.Shadow.Filter       string  PCF   Hard | PCF (3 x 3)
//   r.Shadow.PointLights  bool    1     point lights cast (cube) shadows
//   r.Shadow.Distance     float   100   directional shadows reach this far from the camera (world units)
//   sg.ShadowQuality      int     3     0..4, sets the r.Shadow.* row of the scalability table
//   sg.LightingQuality    int     3     0..4, sets r.Lighting.MaxLights / r.Lighting.Specular
//
// Reference mode forces the settings.json values (referenceModeValues) at CVarSource::Code; every cvar it changes is
// logged with its previous value and source. A Console-set value outranks Code (spec §8) and cannot be forced: that is
// logged as a warning. Punctual lights and shadows are not forced through cvars (that would leave normal mode without
// them after reference mode is turned off): effectiveLighting() derives them from r.ReferenceMode.
#include <engine/CVar.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace eng::renderer {

struct RenderCVars {
    CVar* referenceMode;
    CVar* environment;
    CVar* rotation;
    CVar* intensity;
    CVar* resolution;
    CVar* specularSamples;
    CVar* diffuseSamples;
    CVar* exposure;
    CVar* tonemap;
    CVar* background;
    CVar* backgroundColor;
    CVar* backgroundBlur;
    CVar* referencePunctual;
    CVar* punctual;
    CVar* maxLights;
    CVar* specular;
    CVar* shadowEnable;
    CVar* cascadeCount;
    CVar* shadowResolution;
    CVar* shadowFilter;
    CVar* shadowPointLights;
    CVar* shadowDistance;
    CVar* sgShadowQuality;
    CVar* sgLightingQuality;
};
// Registers the cvars on the first call (process-wide registry).
const RenderCVars& renderCVars();

// Tonemap operator (Tonemap.cpp): the shader code of Frame.frameFlags.y.
enum class TonemapOp : uint32_t { PbrNeutral = 0, Aces = 1, None = 2 };
std::optional<TonemapOp> parseTonemap(std::string_view name);
// r.Tonemap, PbrNeutral with a warning (once per bad value) when it names no operator.
TonemapOp tonemapFromCVar();

struct ReferenceValue {
    std::string_view cvar;
    std::string_view value;
    std::string_view source; // the settings.json key / Sample-Renderer constant it reproduces
};
std::span<const ReferenceValue> referenceModeValues();

// Punctual lights / shadows of the next frame (plan F3 Task 7): r.Lighting.* / r.Shadow.* in normal mode; in reference
// mode punctual = r.ReferenceMode.Punctual and no shadows.
struct LightingSettings {
    bool punctual = true;
    uint32_t maxLights = 64;
    bool specular = true;
    bool shadows = true;
    uint32_t cascadeCount = 3;
    uint32_t shadowResolution = 2048;
    bool pcf = true;
    bool pointShadows = true;
    float shadowDistance = 100.0f;
};
LightingSettings effectiveLighting();

// Scalability table (Config/DefaultScalability.ini, embedded): the cvar values of group `group` ("ShadowQuality",
// "LightingQuality") at `level`. Empty for an unknown group / level.
std::vector<std::pair<std::string, std::string>> scalabilityRow(std::string_view group, int level);

// With r.ReferenceMode = 1: sets every referenceModeValues() entry at CVarSource::Code. Returns one line per cvar it
// changed ("r.Exposure 2 (CommandLine) -> 1") or could not change (Console); empty with r.ReferenceMode = 0.
std::vector<std::string> applyReferenceMode();

} // namespace eng::renderer
