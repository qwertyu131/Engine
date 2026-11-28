// Fit camera (plan F3 Task 4): glTF-Sample-Renderer v1.1.0 (d59d1d6) UserCamera.resetView (user_camera.js):
//   transform = identity, rotation (0, 0) -> look direction -Z;
//   fitDistanceToExtents:   maxAxisLength = max(max.x - min.x, max.y - min.y);
//                           yZoom = maxAxisLength / 2 / tan(yfov / 2), xZoom = maxAxisLength / 2 / tan(xfov / 2) with
//                           xfov = yfov * aspectRatio; distance = max(xZoom, yZoom);
//   fitCameraTargetToExtents: target = (min + max) / 2; position = target - lookDirection * distance = target + (0, 0,
//                           distance);
//   fitCameraPlanesToExtents: longest = 10 * |max - min|; znear = distance - 0.6 longest, zfar = distance + 0.6 longest,
//                           znear = max(znear, zfar / MaxNearFarRatio) with MaxNearFarRatio = 10000.
// Degenerate inputs (Review Focus 5): no extents -> target (0, 0, 0); a zero-size box -> distance 0 in the formulas;
// both are clamped here to a finite camera one unit away (znear / zfar finite and positive); Task 5 tests them.
#include "Scene.h"

#include <algorithm>
#include <cmath>

namespace eng::scene {

FitCamera fitCamera(const SceneInstance& scene, float aspect, float yfov) {
    constexpr float kMaxNearFarRatio = 10000.0f; // user_camera.js MaxNearFarRatio
    FitCamera c;
    c.yfov = yfov;
    const Vec3 mn = scene.extentsValid ? scene.extentMin : Vec3{0, 0, 0};
    const Vec3 mx = scene.extentsValid ? scene.extentMax : Vec3{0, 0, 0};
    const float maxAxisLength = std::max(mx[0] - mn[0], mx[1] - mn[1]);
    const float xfov = yfov * (aspect > 0.0f && std::isfinite(aspect) ? aspect : 1.0f);
    const float yZoom = maxAxisLength / 2.0f / std::tan(yfov / 2.0f);
    const float xZoom = maxAxisLength / 2.0f / std::tan(xfov / 2.0f);
    float distance = std::max(xZoom, yZoom);
    if (!(distance > 0.0f) || !std::isfinite(distance)) {
        distance = 1.0f; // degenerate extents: a finite camera (Review Focus 5)
    }
    c.target = {(mx[0] + mn[0]) / 2.0f, (mx[1] + mn[1]) / 2.0f, (mx[2] + mn[2]) / 2.0f};
    c.position = {c.target[0], c.target[1], c.target[2] + distance};
    const Vec3 d = sub(mx, mn);
    float longest = 10.0f * std::sqrt(dot(d, d));
    if (!(longest > 0.0f) || !std::isfinite(longest)) {
        longest = 10.0f * distance;
    }
    float znear = distance - longest * 0.6f;
    const float zfar = distance + longest * 0.6f;
    znear = std::max(znear, zfar / kMaxNearFarRatio);
    c.znear = znear;
    c.zfar = zfar;
    return c;
}

} // namespace eng::scene
