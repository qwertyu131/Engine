// Viewer panels (plan F2 Task 6): DeviceCaps table (every field + backend + adapter + tier), FPS and the
// frame-time graph of the last 240 frames, and the CVar list with editing at CVarSource::Console.
#include "Panels.h"

#include <engine/CVar.h>

#include <imgui.h>

#include <algorithm>

#include <cstdio>
#include <map>
#include <string>
#include <string_view>
#include <variant>

namespace eng::viewer {

float FrameStats::averageMs() const {
    if (count == 0) {
        return 0.0f;
    }
    float s = 0.0f;
    for (size_t i = 0; i < count; ++i) {
        s += ms[i];
    }
    return s / static_cast<float>(count);
}

namespace {

void row(const char* name, const std::string& value) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted(name);
    ImGui::TableSetColumnIndex(1);
    // A value wider than the column (long adapter names) is clipped by the table: the full text as a tooltip.
    const bool clipped = ImGui::CalcTextSize(value.c_str()).x > ImGui::GetContentRegionAvail().x;
    ImGui::TextUnformatted(value.c_str());
    if (clipped && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", value.c_str());
    }
}

void capsPanel(const rhi::DeviceCaps& caps) {
    ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(330.0f, 0.0f), ImGuiCond_Always);
    ImGui::Begin("Device", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
    // Every DeviceCaps field (rhi::forEachCapsField, the same list as headless --dump-caps), scrollable.
    if (ImGui::BeginTable("caps", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                          ImVec2(0.0f, 150.0f))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        // Field names fixed to their width, the value column takes the rest of the panel (plan F3 Task 1).
        ImGui::TableSetupColumn("field", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        rhi::forEachCapsField(caps, [](std::string_view name, const rhi::CapsValue& v) {
            std::string value;
            if (const bool* b = std::get_if<bool>(&v)) {
                value = *b ? "yes" : "no";
            } else if (const int64_t* i = std::get_if<int64_t>(&v)) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), name == "vendorId" ? "0x%04llx" : "%lld", static_cast<long long>(*i));
                value = buf;
            } else if (const std::string* str = std::get_if<std::string>(&v)) {
                value = *str;
            } else {
                value = rhi::toString(std::get<rhi::FormatCaps>(v));
            }
            if (name == "tier") {
                value = "T" + value;
            }
            row(std::string(name).c_str(), value);
        });
        ImGui::EndTable();
    }
    ImGui::End();
}

void statsPanel(const FrameStats& stats) {
    ImGui::SetNextWindowPos(ImVec2(10.0f, 200.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(330.0f, 0.0f), ImGuiCond_Always);
    ImGui::Begin("Frame time", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
    const float avg = stats.averageMs();
    ImGui::Text("%.1f fps  %.3f ms (avg of %zu frames)", avg > 0.0f ? 1000.0f / avg : 0.0f, avg, stats.count);
    // Oldest first: the ring starts at `next` once full.
    const int offset = stats.count == kFrameHistory ? static_cast<int>(stats.next) : 0;
    ImGui::PlotLines("##ms", stats.ms.data(), static_cast<int>(stats.count), offset, "frame ms", 0.0f, avg * 3.0f + 0.1f,
                     ImVec2(310.0f, 60.0f));
    ImGui::End();
}

void cvarPanel() {
    ImGui::SetNextWindowPos(ImVec2(10.0f, 320.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(330.0f, 0.0f), ImGuiCond_Always);
    ImGui::Begin("CVars", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
    static std::map<std::string, std::string> edits; // per-cvar text being typed
    CVarRegistry& reg = CVarRegistry::get();
    std::vector<std::string> names;
    reg.forEach([&](const CVar& c) { names.emplace_back(c.name()); });
    // The viewer's own r.Viewer.* cvars first (r.Viewer.ShowUI turns this panel off; it stays at a fixed place, the
    // ctest viewer.click clicks it), then every other cvar in name order.
    std::stable_partition(names.begin(), names.end(), [](const std::string& n) { return n.rfind("r.Viewer.", 0) == 0; });
    for (const std::string& name : names) {
        CVar* c = reg.find(name);
        if (!c) {
            continue;
        }
        ImGui::PushID(name.c_str());
        const CVarValue& v = c->value();
        if (std::holds_alternative<bool>(v)) {
            bool b = c->getBool();
            if (ImGui::Checkbox(name.c_str(), &b)) {
                c->set(b, CVarSource::Console);
            }
        } else if (std::holds_alternative<int32_t>(v)) {
            int i = c->getInt();
            if (ImGui::InputInt(name.c_str(), &i, 1, 10, ImGuiInputTextFlags_EnterReturnsTrue)) {
                c->set(static_cast<int32_t>(i), CVarSource::Console);
            }
        } else if (std::holds_alternative<float>(v)) {
            float f = c->getFloat();
            if (ImGui::InputFloat(name.c_str(), &f, 0.0f, 0.0f, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue)) {
                c->set(f, CVarSource::Console);
            }
        } else {
            std::string& text = edits[name];
            if (text.empty()) {
                text = c->getString();
            }
            char buf[256];
            std::snprintf(buf, sizeof(buf), "%s", text.c_str());
            if (ImGui::InputText(name.c_str(), buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
                c->set(std::string(buf), CVarSource::Console);
                text.clear();
            } else {
                text = buf;
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s\nsource: %s", std::string(c->help()).c_str(),
                              std::string(cvarSourceName(c->source())).c_str());
        }
        ImGui::PopID();
    }
    ImGui::End();
}

void assetPanel(const Renderer& renderer, AssetHandle asset, SceneHandle scene, float x) {
    ImGui::SetNextWindowPos(ImVec2(x, 10.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(330.0f, 240.0f), ImGuiCond_Always);
    ImGui::Begin("Asset", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
    const AssetInfo* info = renderer.assetInfo(asset);
    if (!info) {
        ImGui::TextUnformatted("(no asset)");
        ImGui::End();
        return;
    }
    ImGui::TextWrapped("%s", info->source.c_str());
    if (!info->builtin) {
        const SceneInfo si = renderer.sceneInfo(scene);
        ImGui::Text("scenes %u (default %d), draws %u", info->sceneCount, info->defaultScene, si.drawCount);
    }
    if (info->extensions.empty()) {
        ImGui::TextUnformatted("extensions used: none");
    } else if (ImGui::BeginTable("ext", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                                 ImVec2(0.0f, 150.0f))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("extension", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("status", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("why", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const ExtensionStatus& e : info->extensions) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(e.name.c_str());
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", e.name.c_str());
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(e.loaded ? "loaded" : "ignored");
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(e.note.c_str());
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", e.note.c_str());
            }
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

void cvarSlider(const char* name, int lo, int hi) {
    CVar* c = CVarRegistry::get().find(name);
    if (!c) {
        return;
    }
    int v = c->getInt();
    if (ImGui::SliderInt(name, &v, lo, hi)) {
        c->set(static_cast<int32_t>(v), CVarSource::Console);
    }
}

void cvarCheckbox(const char* name) {
    CVar* c = CVarRegistry::get().find(name);
    if (!c) {
        return;
    }
    bool b = c->getBool();
    if (ImGui::Checkbox(name, &b)) {
        c->set(b, CVarSource::Console);
    }
}

void lightsPanel(Renderer& renderer, SceneHandle scene, float x) {
    ImGui::SetNextWindowPos(ImVec2(x, 260.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(330.0f, 220.0f), ImGuiCond_Always);
    ImGui::Begin("Lights", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
    cvarSlider("sg.ShadowQuality", 0, 4);
    cvarSlider("sg.LightingQuality", 0, 4);
    cvarCheckbox("r.ReferenceMode");
    cvarCheckbox("r.ReferenceMode.Punctual");
    ImGui::Separator();
    const std::vector<LightInfo> lights = renderer.lights(scene);
    if (lights.empty()) {
        ImGui::TextUnformatted("no punctual lights");
    }
    for (uint32_t i = 0; i < lights.size(); ++i) {
        const LightInfo& l = lights[i];
        const char* type = l.type == LightType::Directional ? "directional" : l.type == LightType::Spot ? "spot" : "point";
        char label[160];
        std::snprintf(label, sizeof(label), "%u %s (%s, %.3g)%s##light%u", i, l.name.empty() ? "light" : l.name.c_str(),
                      type, l.intensity, l.fromAsset ? "" : " [added]", i);
        bool on = l.enabled;
        if (ImGui::Checkbox(label, &on)) {
            renderer.setLightEnabled(scene, i, on);
        }
    }
    ImGui::End();
}

} // namespace

void drawScenePanels(Renderer& renderer, AssetHandle asset, SceneHandle scene, float windowWidth) {
    const float x = std::max(350.0f, windowWidth - 340.0f);
    assetPanel(renderer, asset, scene, x);
    lightsPanel(renderer, scene, x);
}

void drawPanels(const rhi::DeviceCaps& caps, const FrameStats& stats) {
    capsPanel(caps);
    statsPanel(stats);
    cvarPanel();
}

} // namespace eng::viewer
