// KHR_texture_basisu (plan F3 Task 3): KTX2 files with Basis Universal ETC1S (BasisLZ) or UASTC payloads, parsed and
// transcoded with the pinned KTX-Software `ktx_read` library and its bundled Basis transcoder
// (cmake/Dependencies.cmake).
//   load (inspectKtx2):   ktxTexture2_CreateFromMemory with KTX_TEXTURE_CREATE_CHECK_GLTF_BASISU_BIT (the file must
//                         satisfy KHR_texture_basisu: 2D, one layer / face, ETC1S or UASTC) and the image data loaded,
//                         so a truncated or inconsistent file is an error here (exit 2, Review Focus 1); the DFD
//                         transfer function (KHR_DF_TRANSFER_SRGB) gives Image::ktx2Srgb; r.Asset.MaxTextureSize
//                         skips larger mip levels, and without enough levels the image is transcoded to RGBA8 and
//                         halved like a PNG;
//   upload (transcodeKtx2): the texture is created again from Image::encoded and transcoded to the target
//                         (Ktx2.h chooses it from the device); levels from Image::ktx2BaseLevel on.
#include "Ktx2.h"

#include "Internal.h"

#include <algorithm>
#include <cstring>
#include <format>

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <ktx.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace eng::asset {

namespace {

constexpr int kExit = 2;

struct Texture {
    ktxTexture2* t = nullptr;
    ~Texture() {
        if (t) {
            ktxTexture2_Destroy(t);
        }
    }
};

Result<void> create(const uint8_t* data, size_t size, const std::string& what, Texture& out) {
    const KTX_error_code rc = ktxTexture2_CreateFromMemory(
        data, size, KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT | KTX_TEXTURE_CREATE_CHECK_GLTF_BASISU_BIT, &out.t);
    if (rc != KTX_SUCCESS || !out.t) {
        return std::unexpected(Error{kExit, std::format("{} (image/ktx2): not a valid KHR_texture_basisu KTX2 file: {}",
                                                        what, ktxErrorString(rc))});
    }
    ktxTexture* base = ktxTexture(out.t);
    if (base->numDimensions != 2 || base->numLayers > 1 || base->numFaces != 1 || base->isArray || base->isCubemap ||
        base->baseDepth > 1 || base->baseWidth == 0 || base->baseHeight == 0 || base->numLevels == 0) {
        return std::unexpected(Error{kExit, std::format("{} (image/ktx2): KHR_texture_basisu requires a 2D texture "
                                                        "(one layer, one face)",
                                                        what)});
    }
    if (!ktxTexture2_NeedsTranscoding(out.t)) {
        return std::unexpected(Error{kExit, std::format("{} (image/ktx2): payload is not Basis Universal (vkFormat {})",
                                                        what, out.t->vkFormat)});
    }
    return {};
}

bool hasAlpha(ktxTexture2* t) {
    const ktx_uint32_t n = ktxTexture2_GetNumComponents(t);
    return n == 2 || n == 4;
}

// vkFormat of the transcoded texture -> target (VkFormat values, Vulkan spec "VkFormat").
bool targetOf(uint32_t vk, TranscodeTarget& t, bool& srgb) {
    switch (vk) {
    case 37: t = TranscodeTarget::RGBA8, srgb = false; return true; // VK_FORMAT_R8G8B8A8_UNORM
    case 43: t = TranscodeTarget::RGBA8, srgb = true; return true;  // VK_FORMAT_R8G8B8A8_SRGB
    case 131: case 133: t = TranscodeTarget::BC1, srgb = false; return true; // BC1_RGB(A)_UNORM_BLOCK
    case 132: case 134: t = TranscodeTarget::BC1, srgb = true; return true;  // BC1_RGB(A)_SRGB_BLOCK
    case 137: t = TranscodeTarget::BC3, srgb = false; return true; // BC3_UNORM_BLOCK
    case 138: t = TranscodeTarget::BC3, srgb = true; return true;  // BC3_SRGB_BLOCK
    case 145: t = TranscodeTarget::BC7, srgb = false; return true; // BC7_UNORM_BLOCK
    case 146: t = TranscodeTarget::BC7, srgb = true; return true;  // BC7_SRGB_BLOCK
    case 151: t = TranscodeTarget::ETC2_RGBA8, srgb = false; return true; // ETC2_R8G8B8A8_UNORM_BLOCK
    case 152: t = TranscodeTarget::ETC2_RGBA8, srgb = true; return true;  // ETC2_R8G8B8A8_SRGB_BLOCK
    case 157: t = TranscodeTarget::ASTC_4x4, srgb = false; return true; // ASTC_4x4_UNORM_BLOCK
    case 158: t = TranscodeTarget::ASTC_4x4, srgb = true; return true;  // ASTC_4x4_SRGB_BLOCK
    default: return false;
    }
}

Result<TranscodedImage> transcode(const uint8_t* data, size_t size, const std::string& what, TranscodeTarget target,
                                  uint32_t baseLevel) {
    Texture tex;
    if (auto r = create(data, size, what, tex); !r) {
        return std::unexpected(r.error());
    }
    ktx_transcode_fmt_e fmt = KTX_TTF_RGBA32;
    ktx_transcode_flags flags = 0;
    switch (target) {
    case TranscodeTarget::RGBA8: fmt = KTX_TTF_RGBA32; break;
    case TranscodeTarget::BC1:
    case TranscodeTarget::BC3:
        // BC1 for opaque images: 3-colour blocks (punch-through alpha) forbidden, basist::
        // cDecodeFlagsBC1ForbidThreeColorBlocks = 8 (basisu_transcoder.h; libktx forwards the flags).
        fmt = hasAlpha(tex.t) ? KTX_TTF_BC3_RGBA : KTX_TTF_BC1_RGB;
        flags = 8;
        break;
    case TranscodeTarget::BC7: fmt = KTX_TTF_BC7_RGBA; break;
    case TranscodeTarget::ETC2_RGBA8: fmt = KTX_TTF_ETC2_RGBA; break;
    case TranscodeTarget::ASTC_4x4: fmt = KTX_TTF_ASTC_4x4_RGBA; break;
    }
    if (const KTX_error_code rc = ktxTexture2_TranscodeBasis(tex.t, fmt, flags); rc != KTX_SUCCESS) {
        return std::unexpected(Error{kExit, std::format("{} (image/ktx2): Basis transcode to {} failed: {}", what,
                                                        toString(target), ktxErrorString(rc))});
    }
    TranscodedImage out;
    if (!targetOf(tex.t->vkFormat, out.target, out.srgb)) {
        return std::unexpected(Error{kExit, std::format("{} (image/ktx2): unexpected transcoded vkFormat {}", what,
                                                        tex.t->vkFormat)});
    }
    out.srgb = ktxTexture2_GetTransferFunction_e(tex.t) == KHR_DF_TRANSFER_SRGB;
    ktxTexture* base = ktxTexture(tex.t);
    const uint8_t* bytes = ktxTexture_GetData(base);
    const size_t total = ktxTexture_GetDataSize(base);
    const bool blocks = out.target != TranscodeTarget::RGBA8;
    for (uint32_t level = baseLevel; level < base->numLevels; ++level) {
        ktx_size_t offset = 0;
        if (ktxTexture_GetImageOffset(base, level, 0, 0, &offset) != KTX_SUCCESS) {
            return std::unexpected(Error{kExit, std::format("{} (image/ktx2): level {} has no image", what, level)});
        }
        const size_t levelSize = ktxTexture_GetImageSize(base, level);
        TranscodedLevel l;
        l.width = std::max(1u, base->baseWidth >> level);
        l.height = std::max(1u, base->baseHeight >> level);
        const size_t expected = blocks ? size_t((l.width + 3) / 4) * ((l.height + 3) / 4) * (out.target == TranscodeTarget::BC1 ? 8u : 16u)
                                       : size_t(l.width) * l.height * 4u;
        if (offset + levelSize > total || levelSize != expected) {
            return std::unexpected(Error{kExit, std::format("{} (image/ktx2): level {} is {} bytes, {} expected", what,
                                                            level, levelSize, expected)});
        }
        l.data.assign(bytes + offset, bytes + offset + levelSize);
        out.levels.push_back(std::move(l));
    }
    return out;
}

} // namespace

const char* toString(TranscodeTarget t) {
    switch (t) {
    case TranscodeTarget::RGBA8: return "RGBA8";
    case TranscodeTarget::BC1: return "BC1";
    case TranscodeTarget::BC3: return "BC3";
    case TranscodeTarget::BC7: return "BC7";
    case TranscodeTarget::ETC2_RGBA8: return "ETC2_RGBA8";
    case TranscodeTarget::ASTC_4x4: return "ASTC_4x4";
    }
    return "?";
}

TranscodeTarget chooseTranscodeTarget(const TranscodeSupport& s, const Image& image) {
    if (image.width % 4 != 0 || image.height % 4 != 0) {
        return TranscodeTarget::RGBA8;
    }
    if (s.bc7) {
        return TranscodeTarget::BC7;
    }
    if (s.bc1bc3) {
        return TranscodeTarget::BC3; // transcodeKtx2 picks BC1 for an image without alpha
    }
    if (s.astc) {
        return TranscodeTarget::ASTC_4x4;
    }
    if (s.etc2) {
        return TranscodeTarget::ETC2_RGBA8;
    }
    return TranscodeTarget::RGBA8;
}

Result<TranscodedImage> transcodeKtx2(const Image& image, TranscodeTarget target) {
    if (!image.ktx2 || image.encoded.empty()) {
        return std::unexpected(Error{kExit, "transcodeKtx2: '" + image.name + "' is not a KTX2 image"});
    }
    return transcode(image.encoded.data(), image.encoded.size(), image.name.empty() ? image.uri : image.name, target,
                     image.ktx2BaseLevel);
}

namespace detail {

Result<void> inspectKtx2(const Context& c, std::span<const std::byte> bytes, const std::string& what, Image& img) {
    const auto* data = reinterpret_cast<const uint8_t*>(bytes.data());
    uint32_t w = 0, h = 0, levels = 0;
    bool srgb = false;
    {
        Texture tex;
        if (auto r = create(data, bytes.size(), what, tex); !r) {
            return std::unexpected(r.error());
        }
        ktxTexture* base = ktxTexture(tex.t);
        w = base->baseWidth;
        h = base->baseHeight;
        levels = base->numLevels;
        srgb = ktxTexture2_GetTransferFunction_e(tex.t) == KHR_DF_TRANSFER_SRGB;
    }
    if (overDecodeBudget(c, what, w, h)) {
        return {};
    }
    if (srgb != img.usedAsSrgb && (img.usedAsSrgb || img.usedAsLinear)) {
        c.warn(std::format("{} (image/ktx2): the DFD transfer function is {} but a material uses it as a {} texture; "
                           "the DFD decides the view",
                           what, srgb ? "sRGB" : "linear", img.usedAsSrgb ? "sRGB" : "linear"));
    }
    img.ktx2Srgb = srgb;
    img.colorSpace = srgb ? ColorSpace::Srgb : ColorSpace::Linear;
    uint32_t skip = 0;
    const uint32_t cap = c.options.maxTextureSize;
    while (skip + 1 < levels && (std::max(1u, w >> skip) > cap || std::max(1u, h >> skip) > cap)) {
        ++skip;
    }
    const uint32_t bw = std::max(1u, w >> skip), bh = std::max(1u, h >> skip);
    if (bw <= cap && bh <= cap) {
        if (skip > 0) {
            c.warn(std::format("{}: {}x{} exceeds r.Asset.MaxTextureSize ({}): the KTX2 mip levels from {} ({}x{}) are "
                               "used",
                               what, w, h, cap, skip, bw, bh));
        }
        img.ktx2 = true;
        img.ktx2BaseLevel = skip;
        img.ktx2Levels = levels - skip;
        img.width = bw;
        img.height = bh;
        img.encoded.assign(data, data + bytes.size());
        return {};
    }
    // Not enough mip levels to fit the cap: RGBA8, halved like a PNG.
    auto t = transcode(data, bytes.size(), what, TranscodeTarget::RGBA8, 0);
    if (!t) {
        return std::unexpected(t.error());
    }
    img.width = t->levels[0].width;
    img.height = t->levels[0].height;
    img.rgba8 = std::move(t->levels[0].data);
    img.decoded = true;
    c.info(std::format("{} (image/ktx2): fewer mip levels than r.Asset.MaxTextureSize needs: transcoded to RGBA8", what));
    fitTextureSize(c, what, img);
    return {};
}

} // namespace detail

} // namespace eng::asset
