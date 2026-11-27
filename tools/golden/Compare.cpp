#include "Compare.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

// FLIP.h defines Max/Min macros and static globals: include it in this translation unit only.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include <FLIP.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace eng::golden {

namespace {

std::vector<float> toRgbFloat(const Image& img) {
    const size_t pixels = static_cast<size_t>(img.width) * static_cast<size_t>(img.height);
    std::vector<float> out(pixels * 3u);
    for (size_t i = 0; i < pixels; ++i) {
        for (size_t c = 0; c < 3; ++c) {
            out[i * 3u + c] = static_cast<float>(img.rgba8[i * 4u + c]) / 255.0f;
        }
    }
    return out;
}

std::vector<float> alphaAsGray(const Image& img) {
    const size_t pixels = static_cast<size_t>(img.width) * static_cast<size_t>(img.height);
    std::vector<float> out(pixels * 3u);
    for (size_t i = 0; i < pixels; ++i) {
        const float a = static_cast<float>(img.rgba8[i * 4u + 3u]) / 255.0f;
        out[i * 3u] = out[i * 3u + 1u] = out[i * 3u + 2u] = a;
    }
    return out;
}

bool hasNonOpaqueAlpha(const Image& img) {
    for (size_t i = 3; i < img.rgba8.size(); i += 4) {
        if (img.rgba8[i] != 255) {
            return true;
        }
    }
    return false;
}

// LDR-FLIP error map of two RGB float images (order: reference, test) into `errorMap` (w x h).
// (FLIP::image is not copyable from a const or temporary image, hence the out parameter.)
void flipMap(const std::vector<float>& refRgb, const std::vector<float>& testRgb, int w, int h, bool srgb,
             FLIP::image<float>& errorMap) {
    FLIP::image<FLIP::color3> refImage;
    FLIP::image<FLIP::color3> testImage;
    refImage.setPixels(refRgb.data(), w, h);
    testImage.setPixels(testRgb.data(), w, h);
    if (srgb) {
        // PNG content is sRGB-encoded; FLIP expects linear RGB input.
        refImage.sRGBToLinearRGB();
        testImage.sRGBToLinearRGB();
    }
    FLIP::Parameters params; // default PPD
    FLIP::evaluate(refImage, testImage, /*useHDR=*/false, params, errorMap);
}

double psnr(const Image& a, const Image& b, size_t channels) {
    const size_t pixels = static_cast<size_t>(a.width) * static_cast<size_t>(a.height);
    double sumSq = 0.0;
    for (size_t i = 0; i < pixels; ++i) {
        for (size_t c = 0; c < channels; ++c) {
            const double d =
                static_cast<double>(a.rgba8[i * 4u + c]) - static_cast<double>(b.rgba8[i * 4u + c]);
            sumSq += d * d;
        }
    }
    const double mse = sumSq / static_cast<double>(pixels * channels);
    if (mse == 0.0) {
        return std::numeric_limits<double>::infinity();
    }
    return 10.0 * std::log10((255.0 * 255.0) / mse);
}

uint8_t toByte(float v) {
    const float c = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    return static_cast<uint8_t>(std::lround(c * 255.0f));
}

bool wellFormed(const Image& img) {
    return img.width > 0 && img.height > 0 &&
           img.rgba8.size() == static_cast<size_t>(img.width) * static_cast<size_t>(img.height) * 4u;
}

} // namespace

std::expected<CompareResult, std::string> compare(const Image& test, const Image& ref, const CompareOptions& options) {
    if (!wellFormed(test) || !wellFormed(ref)) {
        return std::unexpected(std::string("invalid image (empty or inconsistent buffer)"));
    }
    if (test.width != ref.width || test.height != ref.height) {
        return std::unexpected("size mismatch: " + std::to_string(test.width) + "x" +
                               std::to_string(test.height) + " vs " + std::to_string(ref.width) + "x" +
                               std::to_string(ref.height));
    }
    if (test.channels != ref.channels) {
        return std::unexpected("channel mismatch: " + std::to_string(test.channels) + " vs " +
                               std::to_string(ref.channels) + " channels");
    }
    const CompareOptions::Alpha alpha =
        options.alpha.value_or(hasNonOpaqueAlpha(test) || hasNonOpaqueAlpha(ref) ? CompareOptions::Alpha::Compare
                                                                                 : CompareOptions::Alpha::Ignore);

    const int w = test.width;
    const int h = test.height;
    FLIP::image<float> errorMap(w, h, 0.0f);
    flipMap(toRgbFloat(ref), toRgbFloat(test), w, h, /*srgb=*/true, errorMap);
    if (alpha == CompareOptions::Alpha::Compare) {
        // Alpha is linear coverage, not sRGB: compared as a gray image, no transfer function.
        FLIP::image<float> alphaMap(w, h, 0.0f);
        flipMap(alphaAsGray(ref), alphaAsGray(test), w, h, /*srgb=*/false, alphaMap);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                errorMap.set(x, y, std::max(errorMap.get(x, y), alphaMap.get(x, y)));
            }
        }
    }

    double sum = 0.0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            sum += static_cast<double>(errorMap.get(x, y));
        }
    }

    FLIP::image<FLIP::color3> magma(w, h);
    magma.colorMap(errorMap, FLIP::magmaMap); // magma colormap values are sRGB-encoded

    CompareResult result;
    result.flipMean = sum / (static_cast<double>(w) * static_cast<double>(h));
    result.psnr = psnr(test, ref, alpha == CompareOptions::Alpha::Compare ? 4u : 3u);
    result.diffHeatmap.width = w;
    result.diffHeatmap.height = h;
    result.diffHeatmap.channels = 3;
    result.diffHeatmap.rgba8.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const FLIP::color3 c = magma.get(x, y);
            const size_t idx = (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4u;
            uint8_t* d = &result.diffHeatmap.rgba8[idx];
            d[0] = toByte(c.r);
            d[1] = toByte(c.g);
            d[2] = toByte(c.b);
            d[3] = 255;
        }
    }
    return result;
}

} // namespace eng::golden
