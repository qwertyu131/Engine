#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace eng::golden {

// 8-bit image, always stored as RGBA8 (row-major, top-down). `channels` is the channel count
// of the source file (1 = gray, 2 = gray+alpha, 3 = RGB, 4 = RGBA); it is kept so mismatched
// inputs can be rejected instead of silently compared.
struct Image {
    int width = 0;
    int height = 0;
    int channels = 0;
    std::vector<uint8_t> rgba8;
};

// Loads a PNG (any bit depth; 16-bit is reduced to 8-bit). The file is read through
// std::filesystem, so UTF-8 / non-ASCII paths work on Windows.
std::expected<Image, std::string> loadPng(const std::filesystem::path& path);

// Writes `img.channels` channels (1..4) taken from rgba8. Returns false on any failure.
bool savePng(const std::filesystem::path& path, const Image& img);

} // namespace eng::golden
