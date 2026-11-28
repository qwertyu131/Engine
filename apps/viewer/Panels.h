#pragma once
// ImGui panels of the viewer (plan F2 Task 6): DeviceCaps table, FPS + frame-time graph, CVar list.
#include <engine/Renderer.h>
#include <engine/rhi/DeviceCaps.h>

#include <array>
#include <cstddef>

namespace eng::viewer {

inline constexpr size_t kFrameHistory = 240;

struct FrameStats {
    std::array<float, kFrameHistory> ms{}; // ring buffer of frame times
    size_t next = 0;
    size_t count = 0;
    void push(float frameMs) {
        ms[next] = frameMs;
        next = (next + 1) % kFrameHistory;
        count = count < kFrameHistory ? count + 1 : kFrameHistory;
    }
    float averageMs() const;
};

// Draws the panels at fixed positions in the top-left area (the cube stays in the centre).
// CVar edits are applied with CVarSource::Console.
void drawPanels(const rhi::DeviceCaps& caps, const FrameStats& stats);

// Plan F3 Task 8: panels of the loaded scene, along the right edge of a `windowWidth` wide window (so the probes of the
// viewer tests, the window centre and the bottom-right corner, stay clear):
//   Asset    source, scenes, draws, and every extension used: loaded, or ignored with the reason;
//   Lights   the scene's punctual lights with an on / off toggle each (Renderer::setLightEnabled), and the quality
//            settings sg.ShadowQuality / sg.LightingQuality (0..4) and r.ReferenceMode / r.ReferenceMode.Punctual
//            (set at CVarSource::Console, as the CVar panel).
void drawScenePanels(Renderer& renderer, AssetHandle asset, SceneHandle scene, float windowWidth);

} // namespace eng::viewer
