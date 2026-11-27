#include "Compare.h"

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

double psnrRgb(const Image& a, const Image& b) {
    const size_t pixels = static_cast<size_t>(a.width) * static_cast<size_t>(a.height);
    double sumSq = 0.0;
    for (size_t i = 0; i < pixels; ++i) {
        for (size_t c = 0; c < 3; ++c) {
            const double d =
                static_cast<double>(a.rgba8[i * 4u + c]) - static_cast<double>(b.rgba8[i * 4u + c]);
            sumSq += d * d;
        }
    }
    const double mse = sumSq / static_cast<double>(pixels * 3u);
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

std::expected<CompareResult, std::string> compare(const Image& test, const Image& ref) {
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

    const int w = test.width;
    const int h = test.height;
    const std::vector<float> testRgb = toRgbFloat(test);
    const std::vector<float> refRgb = toRgbFloat(ref);

    FLIP::image<FLIP::color3> refImage;
    FLIP::image<FLIP::color3> testImage;
    refImage.setPixels(refRgb.data(), w, h);
    testImage.setPixels(testRgb.data(), w, h);
    // PNG content is sRGB-encoded; FLIP expects linear RGB input.
    refImage.sRGBToLinearRGB();
    testImage.sRGBToLinearRGB();

    FLIP::Parameters params; // default PPD
    FLIP::image<float> errorMap(w, h, 0.0f);
    FLIP::evaluate(refImage, testImage, /*useHDR=*/false, params, errorMap); // order: (reference, test)

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
    result.psnr = psnrRgb(test, ref);
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
