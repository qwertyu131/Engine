// glTF extension policy (plan F3 Global Constraints; spec §7.2 extension list, §11 phase plan; registry status in
// docs/gltf-extensions.md). Phase assignment:
//   F3          KHR_lights_punctual (lights parsed), KHR_xmp_json_ld (metadata only); the decoders of plan F3 Task 3:
//               KHR_draco_mesh_compression (Draco.cpp), EXT_meshopt_compression / KHR_meshopt_compression
//               (Meshopt.cpp), KHR_mesh_quantization (Accessors.cpp), KHR_texture_basisu (Ktx2.cpp),
//               EXT_texture_webp (Webp.cpp);
//   F4          KHR_animation_pointer (animation, spec §11 F4);
//   F6          every KHR_materials_* of §7.2, KHR_texture_transform, KHR_materials_variants (§11 F6), and the
//               render extensions §11 gives no own phase: EXT_mesh_gpu_instancing, KHR_node_visibility,
//               EXT_lights_image_based (optional);
//   F6b         KHR_gaussian_splatting;
//   F16         KHR_interactivity, KHR_node_hoverability, KHR_node_selectability.
// Not in the table (Unknown): archived KHR_materials_pbrSpecularGlossiness / KHR_xmp / KHR_techniques_webgl,
// unlisted KHR_materials_scatter, vendor and draft extensions.
#include "Extensions.h"

#include <algorithm>
#include <array>
#include <format>

namespace eng::asset {

namespace {

struct Entry {
    std::string_view name;
    ExtensionInfo info;
};

using S = ExtensionInfo::Status;

constexpr std::array kTable = {
    Entry{"KHR_lights_punctual", {S::Supported, "F3", "punctual lights (rendered from plan F3 Task 7)"}},
    Entry{"KHR_xmp_json_ld", {S::Supported, "F3", "XMP metadata, no render effect"}},
    Entry{"KHR_draco_mesh_compression", {S::Supported, "F3", "Draco mesh decoder"}},
    Entry{"EXT_meshopt_compression", {S::Supported, "F3", "meshoptimizer buffer decoder"}},
    Entry{"KHR_meshopt_compression", {S::Supported, "F3", "meshoptimizer buffer decoder"}},
    Entry{"KHR_mesh_quantization", {S::Supported, "F3", "quantized vertex attributes"}},
    Entry{"KHR_texture_basisu", {S::Supported, "F3", "KTX2 / Basis Universal textures"}},
    Entry{"EXT_texture_webp", {S::Supported, "F3", "WebP textures"}},
    Entry{"KHR_animation_pointer", {S::LaterPhase, "F4", "animation of arbitrary properties"}},
    Entry{"KHR_materials_anisotropy", {S::LaterPhase, "F6", "material extension"}},
    Entry{"KHR_materials_clearcoat", {S::LaterPhase, "F6", "material extension"}},
    Entry{"KHR_materials_diffuse_transmission", {S::LaterPhase, "F6", "material extension"}},
    Entry{"KHR_materials_dispersion", {S::LaterPhase, "F6", "material extension"}},
    Entry{"KHR_materials_emissive_strength", {S::LaterPhase, "F6", "material extension"}},
    Entry{"KHR_materials_ior", {S::LaterPhase, "F6", "material extension"}},
    Entry{"KHR_materials_iridescence", {S::LaterPhase, "F6", "material extension"}},
    Entry{"KHR_materials_retroreflection", {S::LaterPhase, "F6", "material extension"}},
    Entry{"KHR_materials_sheen", {S::LaterPhase, "F6", "material extension"}},
    Entry{"KHR_materials_specular", {S::LaterPhase, "F6", "material extension"}},
    Entry{"KHR_materials_transmission", {S::LaterPhase, "F6", "material extension"}},
    Entry{"KHR_materials_unlit", {S::LaterPhase, "F6", "material extension"}},
    Entry{"KHR_materials_variants", {S::LaterPhase, "F6", "material variants"}},
    Entry{"KHR_materials_volume", {S::LaterPhase, "F6", "material extension"}},
    Entry{"KHR_texture_transform", {S::LaterPhase, "F6", "texture coordinate transform"}},
    Entry{"EXT_mesh_gpu_instancing", {S::LaterPhase, "F6", "GPU instancing"}},
    Entry{"KHR_node_visibility", {S::LaterPhase, "F6", "node visibility"}},
    Entry{"EXT_lights_image_based", {S::LaterPhase, "F6", "image-based lights (optional)"}},
    Entry{"KHR_gaussian_splatting", {S::LaterPhase, "F6b", "Gaussian splats"}},
    Entry{"KHR_interactivity", {S::LaterPhase, "F16", "behaviour graphs"}},
    Entry{"KHR_node_hoverability", {S::LaterPhase, "F16", "hover events"}},
    Entry{"KHR_node_selectability", {S::LaterPhase, "F16", "selection events"}},
};

std::vector<std::string> names(const nlohmann::json& doc, const char* key) {
    std::vector<std::string> out;
    const auto it = doc.find(key);
    if (it != doc.end() && it->is_array()) {
        for (const auto& v : *it) {
            if (v.is_string()) {
                out.push_back(v.get<std::string>());
            }
        }
    }
    return out;
}

} // namespace

ExtensionInfo extensionInfo(std::string_view name) {
    const auto it = std::find_if(kTable.begin(), kTable.end(), [&](const Entry& e) { return e.name == name; });
    return it == kTable.end() ? ExtensionInfo{} : it->info;
}

Result<void> checkExtensions(const nlohmann::json& doc, std::vector<Diagnostic>* diags) {
    const std::vector<std::string> required = names(doc, "extensionsRequired");
    for (const std::string& e : required) {
        const ExtensionInfo i = extensionInfo(e);
        switch (i.status) {
        case S::Supported: break;
        case S::LaterPhase: return std::unexpected(Error{2, std::format("{} is supported from {}", e, i.phase)});
        case S::Unknown: return std::unexpected(Error{2, "unsupported required extension " + e});
        }
    }
    if (!diags) {
        return {};
    }
    for (const std::string& e : names(doc, "extensionsUsed")) {
        const ExtensionInfo i = extensionInfo(e);
        switch (i.status) {
        case S::Supported: break;
        case S::LaterPhase:
            diags->push_back(
                {Diagnostic::Info, std::format("{} ({}): parsed and ignored, supported from {}", e, i.purpose, i.phase)});
            break;
        case S::Unknown:
            diags->push_back({Diagnostic::Warning, std::format("extension {} is not supported: ignored", e)});
            break;
        }
    }
    return {};
}

} // namespace eng::asset
