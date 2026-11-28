// EXT_texture_webp (plan F3 Task 3): WebP images (lossy, lossless, with or without alpha) decoded to RGBA8 with the
// pinned libwebp decoder (cmake/Dependencies.cmake), rows top-down, like PNG / JPEG in Images.cpp. Animated WebP
// (ANIM chunks) is not a glTF texture: the decoder refuses it. The decode budget and r.Asset.MaxTextureSize apply as
// for PNG / JPEG. A file that does not decode is exit 2 (Review Focus 1).
#include "Internal.h"

#include <format>

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <webp/decode.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace eng::asset::detail {

Result<void> decodeWebp(const Context& c, std::span<const std::byte> bytes, const std::string& what, Image& img) {
    const auto* data = reinterpret_cast<const uint8_t*>(bytes.data());
    WebPBitstreamFeatures features{};
    if (WebPGetFeatures(data, bytes.size(), &features) != VP8_STATUS_OK || features.width <= 0 || features.height <= 0) {
        return std::unexpected(fail(std::format("{} ({}): cannot decode: not a valid WebP bitstream", what, img.mimeType)));
    }
    if (features.has_animation) {
        return std::unexpected(fail(std::format("{} ({}): animated WebP is not a texture", what, img.mimeType)));
    }
    if (overDecodeBudget(c, what, uint64_t(features.width), uint64_t(features.height))) {
        return {};
    }
    const size_t stride = size_t(features.width) * 4u;
    std::vector<uint8_t> px(stride * size_t(features.height));
    if (!WebPDecodeRGBAInto(data, bytes.size(), px.data(), px.size(), static_cast<int>(stride))) {
        return std::unexpected(fail(std::format("{} ({}): cannot decode: corrupt or truncated WebP data", what,
                                                img.mimeType)));
    }
    img.width = static_cast<uint32_t>(features.width);
    img.height = static_cast<uint32_t>(features.height);
    img.rgba8 = std::move(px);
    img.decoded = true;
    fitTextureSize(c, what, img);
    return {};
}

} // namespace eng::asset::detail
