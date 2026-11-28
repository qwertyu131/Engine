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
// Reference mode forces the settings.json values (referenceModeValues) at CVarSource::Code; every cvar it changes is
// logged with its previous value and source. A Console-set value outranks Code (spec §8) and cannot be forced: that is
// logged as a warning.
#include <engine/CVar.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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

// With r.ReferenceMode = 1: sets every referenceModeValues() entry at CVarSource::Code. Returns one line per cvar it
// changed ("r.Exposure 2 (CommandLine) -> 1") or could not change (Console); empty with r.ReferenceMode = 0.
std::vector<std::string> applyReferenceMode();

} // namespace eng::renderer
