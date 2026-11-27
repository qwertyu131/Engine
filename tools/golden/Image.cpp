#include "Image.h"

#include <climits>
#include <cstring>
#include <fstream>
#include <iterator>

// Third-party single-header implementations live in this translation unit only.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO // all file IO goes through std::filesystem (UTF-8 paths)
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include <stb_image_write.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include "Utf8.h"

namespace eng::golden {

std::expected<Image, std::string> loadPng(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::unexpected("cannot open '" + toUtf8(path) + "'");
    }
    const std::vector<char> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    if (in.bad()) {
        return std::unexpected("cannot read '" + toUtf8(path) + "'");
    }
    if (bytes.empty() || bytes.size() > static_cast<size_t>(INT_MAX)) {
        return std::unexpected("cannot decode '" + toUtf8(path) + "': empty or too large");
    }

    int w = 0;
    int h = 0;
    int comp = 0;
    stbi_uc* px = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()),
                                        static_cast<int>(bytes.size()), &w, &h, &comp, 4);
    if (px == nullptr) {
        return std::unexpected("cannot decode '" + toUtf8(path) + "': " + stbi_failure_reason());
    }
    Image img;
    img.width = w;
    img.height = h;
    img.channels = comp;
    img.rgba8.assign(px, px + static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
    stbi_image_free(px);
    return img;
}

namespace {
void appendToVector(void* ctx, void* data, int size) {
    auto* out = static_cast<std::vector<unsigned char>*>(ctx);
    const auto* p = static_cast<const unsigned char*>(data);
    out->insert(out->end(), p, p + size);
}
} // namespace

bool savePng(const std::filesystem::path& path, const Image& img) {
    if (img.width <= 0 || img.height <= 0 || img.channels < 1 || img.channels > 4 ||
        img.rgba8.size() != static_cast<size_t>(img.width) * static_cast<size_t>(img.height) * 4u) {
        return false;
    }
    const size_t n = static_cast<size_t>(img.channels);
    const size_t pixels = static_cast<size_t>(img.width) * static_cast<size_t>(img.height);
    std::vector<unsigned char> packed(pixels * n);
    for (size_t i = 0; i < pixels; ++i) {
        const uint8_t* s = &img.rgba8[i * 4u];
        unsigned char* d = &packed[i * n];
        switch (n) {
            case 1: d[0] = s[0]; break;
            case 2: d[0] = s[0]; d[1] = s[3]; break;
            case 3: std::memcpy(d, s, 3); break;
            default: std::memcpy(d, s, 4); break;
        }
    }
    std::vector<unsigned char> encoded;
    if (stbi_write_png_to_func(&appendToVector, &encoded, img.width, img.height, img.channels, packed.data(),
                               img.width * img.channels) == 0) {
        return false;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
    return static_cast<bool>(out);
}

} // namespace eng::golden
