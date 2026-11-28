#pragma once
// Facade camera (spec K-D, plan F3 Task 4): a right-handed perspective look-at camera in world space (+Y up, the
// glTF convention). The aspect ratio is the render target's at draw time.
#include <array>

namespace eng {

struct Camera {
    std::array<float, 3> position{0.0f, 0.0f, 1.0f};
    std::array<float, 3> target{0.0f, 0.0f, 0.0f};
    std::array<float, 3> up{0.0f, 1.0f, 0.0f};
    float yfov = 0.785398163f; // radians (45 deg, the Sample-Viewer fit camera)
    float znear = 0.01f;
    float zfar = 100.0f;
};

} // namespace eng
