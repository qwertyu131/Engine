// glTF image -> GPU texture (plan F3 Task 3). See TextureUpload.h.
#include "TextureUpload.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace eng::renderer {

namespace {

float srgbToLinear(float v) {
    return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}

float linearToSrgb(float v) {
    return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}

// Next mip level of an RGBA8 image: 2 x 2 box filter (an odd edge clamps), colour channels in linear light for sRGB.
std::vector<uint8_t> downsample(const std::vector<uint8_t>& src, uint32_t w, uint32_t h, bool srgb) {
    const uint32_t nw = std::max(1u, w / 2), nh = std::max(1u, h / 2);
    std::vector<uint8_t> dst(size_t(nw) * nh * 4u);
    for (uint32_t y = 0; y < nh; ++y) {
        for (uint32_t x = 0; x < nw; ++x) {
            for (uint32_t c = 0; c < 4; ++c) {
                float sum = 0.0f;
                for (uint32_t dy = 0; dy < 2; ++dy) {
                    for (uint32_t dx = 0; dx < 2; ++dx) {
                        const uint32_t sx = std::min(w - 1, 2 * x + dx), sy = std::min(h - 1, 2 * y + dy);
                        const float v = src[(size_t(sy) * w + sx) * 4u + c] / 255.0f;
                        sum += srgb && c < 3 ? srgbToLinear(v) : v;
                    }
                }
                const float avg = sum * 0.25f;
                const float o = srgb && c < 3 ? linearToSrgb(avg) : avg;
                dst[(size_t(y) * nw + x) * 4u + c] = static_cast<uint8_t>(std::lround(std::clamp(o, 0.0f, 1.0f) * 255.0f));
            }
        }
    }
    return dst;
}

} // namespace

asset::TranscodeSupport transcodeSupport(const rhi::DeviceCaps& caps) {
    using rhi::Format;
    const auto both = [&](Format a, Format b) {
        return caps.format(a).sample && caps.format(a).filter && caps.format(b).sample && caps.format(b).filter;
    };
    asset::TranscodeSupport s;
    s.bc7 = caps.bc6h7 && both(Format::BC7_UNORM, Format::BC7_SRGB);
    s.bc1bc3 = caps.bc && both(Format::BC1_UNORM, Format::BC1_SRGB) && both(Format::BC3_UNORM, Format::BC3_SRGB);
    s.astc = caps.astc && both(Format::ASTC_4x4_UNORM, Format::ASTC_4x4_SRGB);
    s.etc2 = caps.etc2 && both(Format::ETC2_RGBA8_UNORM, Format::ETC2_RGBA8_SRGB);
    return s;
}

rhi::Format gpuFormat(asset::TranscodeTarget target, bool srgb) {
    using rhi::Format;
    switch (target) {
    case asset::TranscodeTarget::RGBA8: return srgb ? Format::RGBA8_SRGB : Format::RGBA8_UNORM;
    case asset::TranscodeTarget::BC1: return srgb ? Format::BC1_SRGB : Format::BC1_UNORM;
    case asset::TranscodeTarget::BC3: return srgb ? Format::BC3_SRGB : Format::BC3_UNORM;
    case asset::TranscodeTarget::BC7: return srgb ? Format::BC7_SRGB : Format::BC7_UNORM;
    case asset::TranscodeTarget::ETC2_RGBA8: return srgb ? Format::ETC2_RGBA8_SRGB : Format::ETC2_RGBA8_UNORM;
    case asset::TranscodeTarget::ASTC_4x4: return srgb ? Format::ASTC_4x4_SRGB : Format::ASTC_4x4_UNORM;
    }
    return Format::Unknown;
}

Result<TextureData> prepareTexture(const asset::Image& image, const rhi::DeviceCaps& caps, bool srgbView) {
    const std::string name = image.name.empty() ? image.uri : image.name;
    TextureData out;
    out.desc.usage = rhi::TextureUsage::Sampled;
    out.desc.debugName = name;
    if (image.ktx2) {
        const asset::TranscodeTarget target = asset::chooseTranscodeTarget(transcodeSupport(caps), image);
        auto t = asset::transcodeKtx2(image, target);
        if (!t) {
            return std::unexpected(t.error());
        }
        out.desc.format = gpuFormat(t->target, t->srgb);
        out.desc.width = t->levels.front().width;
        out.desc.height = t->levels.front().height;
        out.desc.mipLevels = static_cast<uint32_t>(t->levels.size());
        for (asset::TranscodedLevel& l : t->levels) {
            out.levels.push_back(std::move(l.data));
        }
        return out;
    }
    if (!image.decoded || image.width == 0 || image.height == 0) {
        return std::unexpected(Error{2, std::format("image '{}' has no pixels (not decoded)", name)});
    }
    out.desc.format = srgbView ? rhi::Format::RGBA8_SRGB : rhi::Format::RGBA8_UNORM;
    out.desc.width = image.width;
    out.desc.height = image.height;
    out.levels.push_back(image.rgba8);
    uint32_t w = image.width, h = image.height;
    while (w > 1 || h > 1) {
        out.levels.push_back(downsample(out.levels.back(), w, h, srgbView));
        w = std::max(1u, w / 2);
        h = std::max(1u, h / 2);
    }
    out.desc.mipLevels = static_cast<uint32_t>(out.levels.size());
    return out;
}

Result<rhi::TextureHandle> createTexture(rhi::IDevice& device, const TextureData& data) {
    std::vector<rhi::SubresourceData> sub;
    for (const auto& l : data.levels) {
        sub.push_back({std::as_bytes(std::span<const uint8_t>(l)), 0});
    }
    return device.createTexture(data.desc, sub);
}

} // namespace eng::renderer
