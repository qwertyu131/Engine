// Tonemap operator selection (plan F3 Task 5, r.Tonemap). The operators themselves are shaders/post/tonemap.hlsl (a
// by-value port of glTF-Sample-Renderer v1.1.0 tonemapping.glsl), applied in the fragment shaders before blending as
// Sample-Renderer does; see that file for why there is no separate tonemap pass before the F8 frame graph.
#include "RenderSettings.h"

#include "core/Log.h"

#include <set>
#include <string>

namespace eng::renderer {

std::optional<TonemapOp> parseTonemap(std::string_view name) {
    if (name == "PbrNeutral") {
        return TonemapOp::PbrNeutral;
    }
    if (name == "Aces") {
        return TonemapOp::Aces;
    }
    if (name == "None") {
        return TonemapOp::None;
    }
    return std::nullopt;
}

TonemapOp tonemapFromCVar() {
    const std::string& v = renderCVars().tonemap->getString();
    if (auto op = parseTonemap(v)) {
        return *op;
    }
    static std::set<std::string> warned;
    if (warned.insert(v).second) {
        ENG_LOG_WARN("r.Tonemap '{}' is not PbrNeutral | Aces | None; PbrNeutral is used", v);
    }
    return TonemapOp::PbrNeutral;
}

} // namespace eng::renderer
