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
    case Format::BC1_UNORM: return "BC1_UNORM";
    case Format::BC1_SRGB: return "BC1_SRGB";
    case Format::BC2_UNORM: return "BC2_UNORM";
    case Format::BC2_SRGB: return "BC2_SRGB";
    case Format::BC3_UNORM: return "BC3_UNORM";
    case Format::BC3_SRGB: return "BC3_SRGB";
    case Format::BC4_UNORM: return "BC4_UNORM";
    case Format::BC5_UNORM: return "BC5_UNORM";
    case Format::BC6H_UFLOAT: return "BC6H_UFLOAT";
    case Format::BC7_UNORM: return "BC7_UNORM";
    case Format::BC7_SRGB: return "BC7_SRGB";
    case Format::ETC2_RGBA8_UNORM: return "ETC2_RGBA8_UNORM";
    case Format::ETC2_RGBA8_SRGB: return "ETC2_RGBA8_SRGB";
    case Format::ASTC_4x4_UNORM: return "ASTC_4x4_UNORM";
    case Format::ASTC_4x4_SRGB: return "ASTC_4x4_SRGB";
    }
    return "Unknown";
}

std::span<const Format> allFormats() {
    static constexpr Format kAll[] = {
        Format::RGBA8_UNORM, Format::RGBA8_SRGB,  Format::BGRA8_UNORM, Format::BGRA8_SRGB, Format::RGBA16_FLOAT,
        Format::RGBA32_FLOAT, Format::D32_FLOAT,  Format::D24_UNORM_S8_UINT, Format::R32_FLOAT, Format::RG32_FLOAT,
        Format::RGB32_FLOAT, Format::R32_UINT,    Format::RG32_UINT,   Format::RGB32_UINT, Format::RGBA32_UINT,
        Format::R32_SINT,    Format::RGBA32_SINT, Format::RGBA16_UINT, Format::RGBA8_UINT,
        Format::BC1_UNORM,   Format::BC1_SRGB,    Format::BC2_UNORM,   Format::BC2_SRGB,   Format::BC3_UNORM,
        Format::BC3_SRGB,    Format::BC4_UNORM,   Format::BC5_UNORM,   Format::BC6H_UFLOAT, Format::BC7_UNORM,
        Format::BC7_SRGB,    Format::ETC2_RGBA8_UNORM, Format::ETC2_RGBA8_SRGB, Format::ASTC_4x4_UNORM,
        Format::ASTC_4x4_SRGB};
    static_assert(std::size(kAll) == static_cast<size_t>(Format::ASTC_4x4_SRGB), "allFormats(): a Format is missing");
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
    case Format::BC1_UNORM:
    case Format::BC1_SRGB:
    case Format::BC4_UNORM: return 8; // per 4 x 4 block
    case Format::BC2_UNORM:
    case Format::BC2_SRGB:
    case Format::BC3_UNORM:
    case Format::BC3_SRGB:
    case Format::BC5_UNORM:
    case Format::BC6H_UFLOAT:
    case Format::BC7_UNORM:
    case Format::BC7_SRGB:
    case Format::ETC2_RGBA8_UNORM:
    case Format::ETC2_RGBA8_SRGB:
    case Format::ASTC_4x4_UNORM:
    case Format::ASTC_4x4_SRGB: return 16; // per 4 x 4 block
    }
    return 0;
}

FormatBlock formatBlock(Format f) {
    const uint32_t bytes = formatBytes(f);
    return static_cast<uint32_t>(f) >= static_cast<uint32_t>(Format::BC1_UNORM) ? FormatBlock{4, 4, bytes}
                                                                                 : FormatBlock{1, 1, bytes};
}

bool isSrgbFormat(Format f) {
    switch (f) {
    case Format::RGBA8_SRGB:
    case Format::BGRA8_SRGB:
    case Format::BC1_SRGB:
    case Format::BC2_SRGB:
    case Format::BC3_SRGB:
    case Format::BC7_SRGB:
    case Format::ETC2_RGBA8_SRGB:
    case Format::ASTC_4x4_SRGB: return true;
    default: return false;
    }
}

} // namespace eng::rhi
