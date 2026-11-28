// Renderer cvars and reference mode (plan F3 Task 5). See RenderSettings.h.
#include "RenderSettings.h"

#include "core/Log.h"
#include "ScalabilityIni.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <map>

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

// Config/DefaultScalability.ini: "[Group@N]" sections of "name=value" lines; ';' / '#' start a comment line.
struct ScalabilityTable {
    std::map<std::pair<std::string, int>, std::vector<std::pair<std::string, std::string>>> rows;
};
std::string_view trim(std::string_view v) {
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t' || v.front() == '\r')) {
        v.remove_prefix(1);
    }
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r')) {
        v.remove_suffix(1);
    }
    return v;
}
const ScalabilityTable& scalabilityTable() {
    static const ScalabilityTable table = [] {
        ScalabilityTable t;
        std::string_view text(kDefaultScalabilityIni);
        std::pair<std::string, int> section{"", -1};
        while (!text.empty()) {
            const size_t nl = text.find('\n');
            std::string_view line = trim(text.substr(0, nl));
            text = nl == std::string_view::npos ? std::string_view() : text.substr(nl + 1);
            if (line.empty() || line.front() == ';' || line.front() == '#') {
                continue;
            }
            if (line.front() == '[' && line.back() == ']') {
                const std::string_view body = line.substr(1, line.size() - 2);
                const size_t at = body.find('@');
                int level = -1;
                if (at != std::string_view::npos) {
                    const std::string_view lv = body.substr(at + 1);
                    (void)std::from_chars(lv.data(), lv.data() + lv.size(), level);
                }
                section = {std::string(body.substr(0, at)), level};
                if (level < 0) {
                    ENG_LOG_WARN("DefaultScalability.ini: section [{}] is not <Group>@<level>", body);
                }
                continue;
            }
            const size_t eq = line.find('=');
            if (eq == std::string_view::npos || section.second < 0) {
                ENG_LOG_WARN("DefaultScalability.ini: line '{}' ignored", line);
                continue;
            }
            t.rows[section].emplace_back(std::string(trim(line.substr(0, eq))), std::string(trim(line.substr(eq + 1))));
        }
        return t;
    }();
    return table;
}

// sg.<Group> changed: its row at CVarSource::Scalability (a cvar set by a higher source keeps its value).
void applyScalability(std::string_view group, int level) {
    CVarRegistry& r = CVarRegistry::get();
    for (const auto& [name, value] : scalabilityRow(group, level)) {
        CVar* c = r.find(name);
        if (!c) {
            ENG_LOG_WARN("sg.{}={}: unknown cvar {}", group, level, name);
            continue;
        }
        if (c->source() > CVarSource::Scalability) {
            continue; // an explicit value outranks the group
        }
        if (!r.set(name, value, CVarSource::Scalability)) {
            ENG_LOG_WARN("sg.{}={}: {}={} rejected", group, level, name, value);
        }
    }
}

} // namespace

std::vector<std::pair<std::string, std::string>> scalabilityRow(std::string_view group, int level) {
    const ScalabilityTable& t = scalabilityTable();
    auto it = t.rows.find({std::string(group), level});
    return it == t.rows.end() ? std::vector<std::pair<std::string, std::string>>{} : it->second;
}

LightingSettings effectiveLighting() {
    const RenderCVars& c = renderCVars();
    LightingSettings l;
    const bool reference = c.referenceMode->getBool();
    l.punctual = reference ? c.referencePunctual->getBool() : c.punctual->getBool();
    l.maxLights = static_cast<uint32_t>(std::max(0, c.maxLights->getInt()));
    l.specular = c.specular->getBool();
    l.shadows = !reference && c.shadowEnable->getBool();
    l.cascadeCount = static_cast<uint32_t>(std::clamp(c.cascadeCount->getInt(), 1, 4));
    l.shadowResolution = static_cast<uint32_t>(std::max(16, c.shadowResolution->getInt()));
    const std::string& f = c.shadowFilter->getString();
    l.pcf = f != "Hard";
    if (f != "Hard" && f != "PCF") {
        static std::string warned;
        if (warned != f) {
            ENG_LOG_WARN("r.Shadow.Filter '{}' is not Hard or PCF; PCF is used", f);
            warned = f;
        }
    }
    l.pointShadows = c.shadowPointLights->getBool();
    l.shadowDistance = std::max(0.01f, c.shadowDistance->getFloat());
    return l;
}

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
        const auto boolean = [&](const char* name, bool def, const char* help) {
            return r.find(name) ? r.find(name) : r.registerBool(name, def, help);
        };
        // Lighting / shadows (plan F3 Task 7). Defaults = the sg.* level 3 rows of Config/DefaultScalability.ini.
        c.referencePunctual = boolean("r.ReferenceMode.Punctual", false,
                                      "Reference mode: draw KHR_lights_punctual lights (Sample-Viewer usePunctual; the "
                                      "references of tests/golden/ref/sample-viewer have it off)");
        c.punctual = boolean("r.Lighting.Punctual", true, "Normal mode: draw the scene's punctual lights");
        c.maxLights = integer("r.Lighting.MaxLights", 64,
                              "Punctual lights per object (forward light loop; T0 caps at 8, T1 / T2 at 64)", 0, 1024);
        c.specular = boolean("r.Lighting.Specular", true, "Punctual lights' specular lobe (0: diffuse only)");
        c.shadowEnable = boolean("r.Shadow.Enable", true, "Normal-mode shadows of punctual lights");
        c.cascadeCount = integer("r.Shadow.CascadeCount", 3, "Directional light shadow cascades", 1, 4);
        c.shadowResolution = integer("r.Shadow.MapResolution", 2048, "Shadow map tile edge in texels", 16, 16384);
        c.shadowFilter = str("r.Shadow.Filter", "PCF", "Shadow filter: Hard | PCF (3 x 3)");
        c.shadowPointLights = boolean("r.Shadow.PointLights", true, "Point lights cast cube shadows");
        c.shadowDistance = flt("r.Shadow.Distance", 100.0f, "Directional shadow distance from the camera (world units)",
                               0.01f, std::nullopt);
        c.sgShadowQuality = integer("sg.ShadowQuality", 3,
                                    "Shadow scalability 0..4 (Low..Cinematic): Config/DefaultScalability.ini", 0, 4);
        c.sgLightingQuality = integer("sg.LightingQuality", 3,
                                      "Lighting scalability 0..4 (Low..Cinematic): Config/DefaultScalability.ini", 0, 4);
        // sg.* -> the table row, now and on every change.
        applyScalability("ShadowQuality", c.sgShadowQuality->getInt());
        applyScalability("LightingQuality", c.sgLightingQuality->getInt());
        r.onChanged([](const CVar& changed) {
            const std::string_view n = changed.name();
            if (n.size() > 3 && (n[0] == 's' || n[0] == 'S') && (n[1] == 'g' || n[1] == 'G') && n[2] == '.') {
                applyScalability(n.substr(3), changed.getInt());
            }
        });
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
