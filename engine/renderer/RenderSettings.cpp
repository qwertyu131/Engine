// Renderer cvars and reference mode (plan F3 Task 5). See RenderSettings.h.
#include "RenderSettings.h"

#include "core/Log.h"

#include <charconv>
#include <cmath>
#include <format>

namespace eng::renderer {

namespace {

std::string valueText(const CVar& c) {
    const CVarValue& v = c.value();
    if (const bool* b = std::get_if<bool>(&v)) {
        return *b ? "1" : "0";
    }
    if (const int32_t* i = std::get_if<int32_t>(&v)) {
        return std::to_string(*i);
    }
    if (const float* f = std::get_if<float>(&v)) {
        return std::format("{}", *f);
    }
    return std::get<std::string>(v);
}

// True when the cvar already holds `value` (numbers compared as numbers, strings exactly).
bool holds(const CVar& c, std::string_view value) {
    const CVarValue& v = c.value();
    if (std::holds_alternative<std::string>(v)) {
        return std::get<std::string>(v) == value;
    }
    double want = 0.0;
    const auto [p, ec] = std::from_chars(value.data(), value.data() + value.size(), want);
    if (ec != std::errc() || p != value.data() + value.size()) {
        return false;
    }
    if (const bool* b = std::get_if<bool>(&v)) {
        return (*b ? 1.0 : 0.0) == want;
    }
    if (const int32_t* i = std::get_if<int32_t>(&v)) {
        return double(*i) == want;
    }
    return std::abs(double(std::get<float>(v)) - want) <= 1e-6 * std::max(1.0, std::abs(want));
}

// Values of tests/golden/ref/sample-viewer/settings.json "settings" and of glTF-Sample-Renderer v1.1.0 iblSampler
// (ibl_sampler.js); test_reference_mode.cpp checks them against the committed settings.json.
constexpr ReferenceValue kReference[] = {
    {"r.IBL.Environment", "Cannon_Exterior", "settings.environment"},
    {"r.IBL.Rotation", "90", "settings.environmentRotationDeg"},
    {"r.IBL.Intensity", "1", "settings.iblIntensity (x iblIntensityScale = 1 on the FLOAT path)"},
    {"r.Exposure", "1", "settings.exposure"},
    {"r.Tonemap", "PbrNeutral", "settings.toneMap = Khronos PBR Neutral"},
    {"r.Background", "Environment", "settings.renderEnvironmentMap = true"},
    {"r.Background.Blur", "0.6", "settings.blurEnvironmentMap = true (u_EnvBlurNormalized 0.6)"},
    {"r.IBL.Resolution", "256", "iblSampler.textureSize"},
    {"r.IBL.SpecularSamples", "1024", "iblSampler.ggxSampleCount"},
    {"r.IBL.DiffuseSamples", "2048", "iblSampler.lambertianSampleCount"},
};

} // namespace

const RenderCVars& renderCVars() {
    static const RenderCVars cvars = [] {
        CVarRegistry& r = CVarRegistry::get();
        RenderCVars c{};
        c.referenceMode = r.find("r.ReferenceMode")
                              ? r.find("r.ReferenceMode")
                              : r.registerBool("r.ReferenceMode", false,
                                               "1 = reference mode (spec K-B): reproduce the glTF-Sample-Viewer settings "
                                               "of tests/golden/ref/sample-viewer/settings.json");
        const auto str = [&](const char* name, const char* def, const char* help) {
            return r.find(name) ? r.find(name) : r.registerString(name, def, help);
        };
        const auto flt = [&](const char* name, float def, const char* help, std::optional<float> mn,
                             std::optional<float> mx) {
            return r.find(name) ? r.find(name) : r.registerFloat(name, def, help, CVarFlags::None, mn, mx);
        };
        const auto integer = [&](const char* name, int32_t def, const char* help, int32_t mn, int32_t mx) {
            return r.find(name) ? r.find(name) : r.registerInt(name, def, help, CVarFlags::None, mn, mx);
        };
        c.environment = str("r.IBL.Environment", "Cannon_Exterior",
                            "IBL environment: a name (<assetDir>/environments/<name>.hdr), a .hdr path, or builtin");
        c.rotation = flt("r.IBL.Rotation", 90.0f, "IBL environment rotation about +Y, degrees", std::nullopt,
                         std::nullopt);
        c.intensity = flt("r.IBL.Intensity", 1.0f, "IBL intensity (u_EnvIntensity)", 0.0f, std::nullopt);
        c.resolution = integer("r.IBL.Resolution", 256, "IBL cube edge (iblSampler.textureSize), a power of two", 16,
                               2048);
        c.specularSamples = integer("r.IBL.SpecularSamples", 1024, "GGX prefilter samples (ggxSampleCount)", 1, 65536);
        c.diffuseSamples = integer("r.IBL.DiffuseSamples", 2048, "Lambertian prefilter samples (lambertianSampleCount)",
                                   1, 65536);
        c.exposure = flt("r.Exposure", 1.0f, "Exposure factor before the tonemap (u_Exposure)", 0.0f, std::nullopt);
        c.tonemap = str("r.Tonemap", "PbrNeutral", "Tonemap operator: PbrNeutral | Aces | None");
        c.background = str("r.Background", "Environment",
                           "glTF scenes: Environment (the blurred IBL environment) or Color (r.Background.Color)");
        c.backgroundColor = str("r.Background.Color", "0,0,0",
                                "Background colour with r.Background=Color: r,g,b display values in [0, 1] "
                                "(sRGB-encoded)");
        c.backgroundBlur = flt("r.Background.Blur", 0.6f,
                               "Environment background blur, normalized (u_EnvBlurNormalized; LOD = blur x (mips - 1))",
                               0.0f, 1.0f);
        return c;
    }();
    return cvars;
}

std::span<const ReferenceValue> referenceModeValues() {
    return kReference;
}

std::vector<std::string> applyReferenceMode() {
    const RenderCVars& c = renderCVars();
    std::vector<std::string> lines;
    if (!c.referenceMode->getBool()) {
        return lines;
    }
    CVarRegistry& r = CVarRegistry::get();
    for (const ReferenceValue& v : kReference) {
        CVar* cv = r.find(v.cvar);
        if (!cv || holds(*cv, v.value)) {
            continue;
        }
        const std::string before = valueText(*cv);
        const CVarSource was = cv->source();
        if (r.set(v.cvar, v.value, CVarSource::Code)) {
            lines.push_back(std::format("r.ReferenceMode: {} {} ({}) -> {} ({})", v.cvar, before, cvarSourceName(was),
                                        v.value, v.source));
            ENG_LOG_INFO("{}", lines.back());
        } else {
            lines.push_back(std::format("r.ReferenceMode: {} stays {} (set by {}, which outranks Code); reference "
                                        "value {}",
                                        v.cvar, before, cvarSourceName(was), v.value));
            ENG_LOG_WARN("{}", lines.back());
        }
    }
    return lines;
}

} // namespace eng::renderer
