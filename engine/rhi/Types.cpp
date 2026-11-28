#include "Types.h"

#include <iterator>

namespace eng::rhi {

const char* toString(Backend b) {
    switch (b) {
    case Backend::Vulkan: return "vulkan";
    case Backend::D3D11: return "d3d11";
    }
    return "unknown";
}

const char* toString(Format f) {
    switch (f) {
    case Format::Unknown: return "Unknown";
    case Format::RGBA8_UNORM: return "RGBA8_UNORM";
    case Format::RGBA8_SRGB: return "RGBA8_SRGB";
    case Format::BGRA8_UNORM: return "BGRA8_UNORM";
    case Format::BGRA8_SRGB: return "BGRA8_SRGB";
    case Format::RGBA16_FLOAT: return "RGBA16_FLOAT";
    case Format::RGBA32_FLOAT: return "RGBA32_FLOAT";
    case Format::D32_FLOAT: return "D32_FLOAT";
    case Format::D24_UNORM_S8_UINT: return "D24_UNORM_S8_UINT";
    case Format::R32_FLOAT: return "R32_FLOAT";
    case Format::RG32_FLOAT: return "RG32_FLOAT";
    case Format::RGB32_FLOAT: return "RGB32_FLOAT";
    case Format::R32_UINT: return "R32_UINT";
    case Format::RG32_UINT: return "RG32_UINT";
    case Format::RGB32_UINT: return "RGB32_UINT";
    case Format::RGBA32_UINT: return "RGBA32_UINT";
    case Format::R32_SINT: return "R32_SINT";
    case Format::RGBA32_SINT: return "RGBA32_SINT";
    case Format::RGBA16_UINT: return "RGBA16_UINT";
    case Format::RGBA8_UINT: return "RGBA8_UINT";
    }
    return "Unknown";
}

std::span<const Format> allFormats() {
    static constexpr Format kAll[] = {
        Format::RGBA8_UNORM, Format::RGBA8_SRGB,  Format::BGRA8_UNORM, Format::BGRA8_SRGB, Format::RGBA16_FLOAT,
        Format::RGBA32_FLOAT, Format::D32_FLOAT,  Format::D24_UNORM_S8_UINT, Format::R32_FLOAT, Format::RG32_FLOAT,
        Format::RGB32_FLOAT, Format::R32_UINT,    Format::RG32_UINT,   Format::RGB32_UINT, Format::RGBA32_UINT,
        Format::R32_SINT,    Format::RGBA32_SINT, Format::RGBA16_UINT, Format::RGBA8_UINT};
    static_assert(std::size(kAll) == static_cast<size_t>(Format::RGBA8_UINT), "allFormats(): a Format is missing");
    return kAll;
}

uint32_t formatBytes(Format f) {
    switch (f) {
    case Format::Unknown: return 0;
    case Format::RGBA8_UNORM:
    case Format::RGBA8_SRGB:
    case Format::BGRA8_UNORM:
    case Format::BGRA8_SRGB:
    case Format::D32_FLOAT:
    case Format::D24_UNORM_S8_UINT:
    case Format::R32_FLOAT:
    case Format::R32_UINT:
    case Format::R32_SINT:
    case Format::RGBA8_UINT: return 4;
    case Format::RGBA16_FLOAT:
    case Format::RG32_FLOAT:
    case Format::RG32_UINT:
    case Format::RGBA16_UINT: return 8;
    case Format::RGB32_FLOAT:
    case Format::RGB32_UINT: return 12;
    case Format::RGBA32_FLOAT:
    case Format::RGBA32_UINT:
    case Format::RGBA32_SINT: return 16;
    }
    return 0;
}

} // namespace eng::rhi
