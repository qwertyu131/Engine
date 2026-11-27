#pragma once

#include "Image.h"

#include <expected>
#include <optional>
#include <string>

namespace eng::golden {

struct CompareResult {
    double flipMean = 0.0; // mean LDR-FLIP error in [0,1] (0 = identical)
    double psnr = 0.0;     // dB over RGB; +infinity for identical images
    Image diffHeatmap;     // FLIP error mapped with the magma colormap, RGB (channels = 3)
};

struct CompareOptions {
    enum class Alpha { Ignore, Compare };
    // Unset: Compare when either image has a non-opaque alpha value, otherwise Ignore.
    std::optional<Alpha> alpha;
};

// Compares `test` against `ref` with NVIDIA LDR-FLIP (default PPD ~67.02: 0.7 m from a 0.7 m
// wide 3840 px monitor) and PSNR. RGB is treated as sRGB-encoded.
// Alpha (plan F2 Task 1), when compared: alpha is a second, linear, gray FLIP image; the per-pixel
// error is max(colour FLIP, alpha FLIP), and PSNR covers RGBA instead of RGB.
// Error (never a resize) if the sizes or the source channel counts differ.
std::expected<CompareResult, std::string> compare(const Image& test, const Image& ref, const CompareOptions& options = {});

} // namespace eng::golden
