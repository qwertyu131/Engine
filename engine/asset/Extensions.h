#pragma once
// glTF extension policy of the loader (plan F3 Global Constraints, spec §7.2 / §11). One table: every extension the
// spec names, with the phase that implements it.
//   Supported  handled now (KHR_lights_punctual -> ModelData::lights; KHR_xmp_json_ld: metadata, no render effect);
//   Pending    F3 decoders of plan Task 3 (Draco, meshopt, quantization, KTX2/Basis, WebP): until then required ->
//              exit 2 "<name> is supported from F3 (decoder: plan F3 Task 3)", used -> info, fallback data is used;
//   LaterPhase F4..F16: required -> exit 2 "<name> is supported from F<n>", used -> parsed and ignored, info;
//   Unknown    anything else (incl. archived / unlisted): required -> exit 2 "unsupported required extension
//              <name>", used -> warning, ignored.
#include "GltfLoader.h"

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace eng::asset {

struct ExtensionInfo {
    enum class Status : uint8_t { Supported, Pending, LaterPhase, Unknown };
    Status status = Status::Unknown;
    const char* phase = "";  // "F3", "F4", "F6", "F6b", "F16" ("" when unknown)
    const char* purpose = ""; // one line, for messages / the viewer's asset panel (Task 8)
};

ExtensionInfo extensionInfo(std::string_view name);

// Applies the policy to the document's extensionsRequired / extensionsUsed: the first unsupported required
// extension is an error (exit 2); used ones add diagnostics.
Result<void> checkExtensions(const nlohmann::json& doc, std::vector<Diagnostic>* diags);

} // namespace eng::asset
