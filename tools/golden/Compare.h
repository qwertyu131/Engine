#pragma once

#include "Image.h"

#include <expected>
#include <string>

namespace eng::golden {

struct CompareResult {
    double flipMean = 0.0; // mean LDR-FLIP error in [0,1] (0 = identical)
    double psnr = 0.0;     // dB over RGB; +infinity for identical images
    Image diffHeatmap;     // FLIP error mapped with the magma colormap, RGB (channels = 3)
};

// Compares `test` against `ref` with NVIDIA LDR-FLIP (default PPD ~67.02: 0.7 m from a 0.7 m
// wide 3840 px monitor) and PSNR. Inputs are treated as sRGB-encoded; alpha is ignored.
// Error (never a resize) if the sizes or the source channel counts differ.
std::expected<CompareResult, std::string> compare(const Image& test, const Image& ref);

} // namespace eng::golden
