// Images (plan F3 Task 2): every glTF image is resolved (external file, bufferView, data URI) and decoded to RGBA8:
// PNG with stb_image, JPEG with libjpeg-turbo's TurboJPEG API (both pinned in cmake/Dependencies.cmake; stb_image
// rejects two valid progressive JPEGs of ABeautifulGame, see there). A libjpeg-turbo warning (corrupt but decodable
// data) is a Diagnostic::Warning naming the image; a fatal decoder error is exit 2. KTX2 and WebP keep their payload for the
// decoders of plan F3 Task 3. Colour space from usage (Review Focus 3): baseColor / emissive sRGB, the other
// texture slots linear. r.Asset.MaxTextureSize: a larger image is halved (2 x 2 box filter, in linear light for sRGB
// images) until it fits, with a warning. A PNG / JPEG that fails to decode is an error (exit 2, Review Focus 1).
#include "Internal.h"

#include "core/Log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <stb_image.h>
#include <turbojpeg.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace eng::asset::detail {

namespace {

// Decoded pixel budget: larger images are refused (warning, image left undecoded) instead of allocating gigabytes.
constexpr uint64_t kMaxDecodePixels = uint64_t{16384} * 16384;

std::string sniffMime(std::span<const std::byte> b) {
    const auto at = [&](size_t i) { return i < b.size() ? static_cast<unsigned char>(b[i]) : 0u; };
    if (b.size() >= 8 && at(0) == 0x89 && at(1) == 'P' && at(2) == 'N' && at(3) == 'G') {
        return "image/png";
    }
    if (b.size() >= 3 && at(0) == 0xFF && at(1) == 0xD8 && at(2) == 0xFF) {
        return "image/jpeg";
    }
    if (b.size() >= 12 && at(0) == 0xAB && at(1) == 'K' && at(2) == 'T' && at(3) == 'X' && at(5) == '2' && at(6) == '0') {
        return "image/ktx2";
    }
    if (b.size() >= 12 && at(0) == 'R' && at(1) == 'I' && at(2) == 'F' && at(3) == 'F' && at(8) == 'W' && at(9) == 'E' &&
        at(10) == 'B' && at(11) == 'P') {
        return "image/webp";
    }
    return {};
}

std::string mimeName(fastgltf::MimeType m) {
    switch (m) {
    case fastgltf::MimeType::JPEG: return "image/jpeg";
    case fastgltf::MimeType::PNG: return "image/png";
    case fastgltf::MimeType::KTX2: return "image/ktx2";
    case fastgltf::MimeType::DDS: return "image/vnd-ms.dds";
    case fastgltf::MimeType::WEBP: return "image/webp";
    case fastgltf::MimeType::AVIF: return "image/avif";
    default: return {};
    }
}

float srgbToLinear(float v) {
    return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}

float linearToSrgb(float v) {
    return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}

// One 2 x 2 box-filter halving (odd edges clamp). sRGB colour channels are averaged in linear light.
void halve(Image& img, bool srgb) {
    const uint32_t w = img.width, h = img.height;
    const uint32_t nw = std::max(1u, w / 2), nh = std::max(1u, h / 2);
    std::vector<uint8_t> dst(size_t(nw) * nh * 4u);
    for (uint32_t y = 0; y < nh; ++y) {
        for (uint32_t x = 0; x < nw; ++x) {
            for (uint32_t ch = 0; ch < 4; ++ch) {
                float sum = 0.0f;
                for (uint32_t dy = 0; dy < 2; ++dy) {
                    for (uint32_t dx = 0; dx < 2; ++dx) {
                        const uint32_t sx = std::min(w - 1, 2 * x + dx), sy = std::min(h - 1, 2 * y + dy);
                        const float v = img.rgba8[(size_t(sy) * w + sx) * 4u + ch] / 255.0f;
                        sum += srgb && ch < 3 ? srgbToLinear(v) : v;
                    }
                }
                const float avg = sum * 0.25f;
                const float o = srgb && ch < 3 ? linearToSrgb(avg) : avg;
                dst[(size_t(y) * nw + x) * 4u + ch] = static_cast<uint8_t>(std::lround(std::clamp(o, 0.0f, 1.0f) * 255.0f));
            }
        }
    }
    img.rgba8 = std::move(dst);
    img.width = nw;
    img.height = nh;
}

bool overBudget(const Context& c, const std::string& what, int w, int h) {
    if (static_cast<uint64_t>(w) * static_cast<uint64_t>(h) > kMaxDecodePixels) {
        c.warn(std::format("{}: {}x{} exceeds the decode budget ({} pixels): not decoded", what, w, h, kMaxDecodePixels));
        return true;
    }
    return false;
}

Result<void> decodePng(const Context& c, std::span<const std::byte> bytes, const std::string& what, Image& img) {
    const auto* data = reinterpret_cast<const stbi_uc*>(bytes.data());
    const int len = static_cast<int>(bytes.size());
    int w = 0, h = 0, comp = 0;
    if (!stbi_info_from_memory(data, len, &w, &h, &comp)) {
        return std::unexpected(fail(std::format("{} ({}): cannot decode: {}", what, img.mimeType, stbi_failure_reason())));
    }
    if (overBudget(c, what, w, h)) {
        return {};
    }
    stbi_uc* px = stbi_load_from_memory(data, len, &w, &h, &comp, 4);
    if (!px) {
        return std::unexpected(fail(std::format("{} ({}): cannot decode: {}", what, img.mimeType, stbi_failure_reason())));
    }
    img.width = static_cast<uint32_t>(w);
    img.height = static_cast<uint32_t>(h);
    img.rgba8.assign(px, px + size_t(w) * size_t(h) * 4u);
    stbi_image_free(px);
    img.decoded = true;
    return {};
}

Result<void> decodeJpeg(const Context& c, std::span<const std::byte> bytes, const std::string& what, Image& img) {
    struct Handle {
        tjhandle h = tj3Init(TJINIT_DECOMPRESS);
        ~Handle() { tj3Destroy(h); }
    } tj;
    if (!tj.h) {
        return std::unexpected(fail(what + ": cannot create a TurboJPEG decompressor"));
    }
    const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
    tj3Set(tj.h, TJPARAM_STOPONWARNING, 0);
    if (tj3DecompressHeader(tj.h, data, bytes.size()) != 0) {
        return std::unexpected(fail(std::format("{} ({}): cannot decode: {}", what, img.mimeType, tj3GetErrorStr(tj.h))));
    }
    const int w = tj3Get(tj.h, TJPARAM_JPEGWIDTH), h = tj3Get(tj.h, TJPARAM_JPEGHEIGHT);
    if (w <= 0 || h <= 0) {
        return std::unexpected(fail(std::format("{} ({}): cannot decode: bad size {}x{}", what, img.mimeType, w, h)));
    }
    if (overBudget(c, what, w, h)) {
        return {};
    }
    std::vector<uint8_t> px(size_t(w) * size_t(h) * 4u, 0); // zeroed: a warning may leave rows undecoded
    if (tj3Decompress8(tj.h, data, bytes.size(), px.data(), 0, TJPF_RGBA) != 0) {
        if (tj3GetErrorCode(tj.h) != TJERR_WARNING) {
            return std::unexpected(fail(std::format("{} ({}): cannot decode: {}", what, img.mimeType, tj3GetErrorStr(tj.h))));
        }
        c.warn(std::format("{} ({}): decoded with a libjpeg-turbo warning: {}", what, img.mimeType, tj3GetErrorStr(tj.h)));
    }
    img.width = static_cast<uint32_t>(w);
    img.height = static_cast<uint32_t>(h);
    img.rgba8 = std::move(px);
    img.decoded = true;
    return {};
}

} // namespace

Result<void> loadImages(const Context& c, ModelData& model) {
    const auto& images = c.asset.images;
    model.images.resize(images.size());
    // Usage -> colour space.
    const auto use = [&](const TextureRef& t, bool srgb) {
        if (t.image >= 0 && static_cast<size_t>(t.image) < model.images.size()) {
            (srgb ? model.images[t.image].usedAsSrgb : model.images[t.image].usedAsLinear) = true;
        }
    };
    for (const Material& m : model.materials) {
        use(m.baseColor, true);
        use(m.emissive, true);
        use(m.metallicRoughness, false);
        use(m.normal, false);
        use(m.occlusion, false);
    }
    const nlohmann::json* jimages = nullptr;
    if (const auto it = c.json.find("images"); it != c.json.end() && it->is_array()) {
        jimages = &*it;
    }
    for (size_t i = 0; i < images.size(); ++i) {
        const std::string what = std::format("image {}", i);
        Image& img = model.images[i];
        img.name = images[i].name;
        img.colorSpace = img.usedAsSrgb ? ColorSpace::Srgb : ColorSpace::Linear;
        if (img.usedAsSrgb && img.usedAsLinear) {
            c.warn(what + ": used as an sRGB and as a linear texture; decoded once, flagged sRGB");
        }
        std::vector<std::byte> fileBytes;
        std::span<const std::byte> bytes;
        std::string declared;
        const fastgltf::DataSource& src = images[i].data;
        if (const auto* bv = std::get_if<fastgltf::sources::BufferView>(&src)) {
            auto v = bufferViewBytes(c, bv->bufferViewIndex, what);
            if (!v) {
                return std::unexpected(v.error());
            }
            bytes = *v;
            declared = mimeName(bv->mimeType);
        } else if (const auto* u = std::get_if<fastgltf::sources::URI>(&src)) {
            std::string uri;
            if (jimages && i < jimages->size() && (*jimages)[i].contains("uri") && (*jimages)[i]["uri"].is_string()) {
                uri = (*jimages)[i]["uri"].get<std::string>();
            }
            img.uri = uri;
            auto path = resolveUri(c.baseDir, uri, what);
            if (!path) {
                return std::unexpected(path.error());
            }
            auto data = readFileCapped(*path, c.options.maxFileBytes, what);
            if (!data) {
                return std::unexpected(data.error());
            }
            fileBytes = std::move(*data);
            bytes = fileBytes;
            declared = mimeName(u->mimeType);
        } else if (const auto* a = std::get_if<fastgltf::sources::Array>(&src)) {
            bytes = std::span<const std::byte>(a->bytes.data(), a->bytes.size());
            declared = mimeName(a->mimeType);
            img.uri = "data:";
        } else if (const auto* vec = std::get_if<fastgltf::sources::Vector>(&src)) {
            bytes = vec->bytes;
            declared = mimeName(vec->mimeType);
        } else if (const auto* bvw = std::get_if<fastgltf::sources::ByteView>(&src)) {
            bytes = bvw->bytes;
            declared = mimeName(bvw->mimeType);
        } else {
            return std::unexpected(fail(what + ": no image data"));
        }
        const std::string sniffed = sniffMime(bytes);
        img.mimeType = !sniffed.empty() ? sniffed : declared;
        if (img.mimeType != "image/png" && img.mimeType != "image/jpeg") {
            if (img.mimeType == "image/ktx2" || img.mimeType == "image/webp") {
                img.encoded.assign(reinterpret_cast<const uint8_t*>(bytes.data()),
                                   reinterpret_cast<const uint8_t*>(bytes.data()) + bytes.size());
                c.info(std::format("{} ({}): kept encoded, decoder in plan F3 Task 3", what, img.mimeType));
                continue;
            }
            return std::unexpected(fail(std::format("{}: unrecognised image data ({} bytes, mimeType '{}'): cannot "
                                                    "decode",
                                                    what, bytes.size(), declared)));
        }
        if (bytes.size() > static_cast<size_t>(INT32_MAX)) {
            return std::unexpected(fail(what + ": larger than 2 GiB"));
        }
        auto decoded = img.mimeType == "image/jpeg" ? decodeJpeg(c, bytes, what, img) : decodePng(c, bytes, what, img);
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        if (!img.decoded) {
            continue; // over the decode budget (warned)
        }
        if (img.width > c.options.maxTextureSize || img.height > c.options.maxTextureSize) {
            const uint32_t ow = img.width, oh = img.height;
            while (img.width > c.options.maxTextureSize || img.height > c.options.maxTextureSize) {
                halve(img, img.colorSpace == ColorSpace::Srgb);
            }
            c.warn(std::format("{}: {}x{} exceeds r.Asset.MaxTextureSize ({}): downscaled to {}x{}", what, ow, oh,
                               c.options.maxTextureSize, img.width, img.height));
        }
    }
    return {};
}

} // namespace eng::asset::detail
