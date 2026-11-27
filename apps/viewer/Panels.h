#pragma once
// ImGui panels of the viewer (plan F2 Task 6): DeviceCaps table, FPS + frame-time graph, CVar list.
#include "rhi/DeviceCaps.h"

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

} // namespace eng::viewer
