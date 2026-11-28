// KHR_texture_basisu (plan F3 Task 3): KTX2 files with Basis Universal ETC1S (BasisLZ) or UASTC payloads, parsed and
// transcoded with the pinned KTX-Software `ktx_read` library and its bundled Basis transcoder
// (cmake/Dependencies.cmake).
//   load (inspectKtx2):   checkLayout first (header and level index against the file size, the UASTC size of each
//                         level and r.Asset.MaxFileBytes: libktx allocates what the index declares), then the
//                         decode budget on the declared size (over it: warning, not decoded), then
//                         ktxTexture2_CreateFromMemory with KTX_TEXTURE_CREATE_CHECK_GLTF_BASISU_BIT (the file must
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
#include <cstdint>
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

uint32_t le32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
uint64_t le64(const uint8_t* p) {
    return uint64_t(le32(p)) | uint64_t(le32(p + 4)) << 32;
}
// [offset, offset + length) inside a file of `size` bytes, without wrapping past 2^64.
bool inFile(uint64_t offset, uint64_t length, uint64_t size) {
    return offset <= size && length <= size - offset;
}

// Header + level index check before libktx sees the file (KTX 2.0 spec, sections 3.1-3.9). libktx sizes its level
// index by levelCount, mallocs the DFD / key-value data / SGD lengths and the level data (byteLength of the levels;
// with Zstandard / zlib also the sum of uncompressedByteLength) before it reads them, so a 1 KB file declaring 2^62
// bytes would commit (or, in the ASan build, abort on) that much. Every range is checked against the file size,
// every uncompressedByteLength against the UASTC size of its level (16 bytes per 4 x 4 block, the largest Basis
// payload KHR_texture_basisu allows) and r.Asset.MaxFileBytes, and their sum against r.Asset.MaxFileBytes.
Result<void> checkLayout(const uint8_t* data, size_t size, const std::string& what, uint64_t maxBytes) {
    const auto fail = [&](std::string m) {
        return std::unexpected(Error{kExit, std::format("{} (image/ktx2): {}", what, m)});
    };
    static constexpr uint8_t kId[12] = {0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, '\r', '\n', 0x1A, '\n'};
    constexpr size_t kHeader = 80, kLevelEntry = 24;
    if (size < kHeader || std::memcmp(data, kId, sizeof(kId)) != 0) {
        return fail(std::format("not a KTX2 file ({} bytes, no KTX 2.0 identifier and header)", size));
    }
    const uint64_t fileSize = size;
    const uint32_t w = le32(data + 20), h = le32(data + 24), depth = le32(data + 28), layers = le32(data + 32),
                   faces = le32(data + 36), levelCount = le32(data + 40), scheme = le32(data + 44);
    if (w == 0 || depth != 0 || layers > 1 || faces != 1) {
        return fail(std::format("KHR_texture_basisu requires a 2D texture (one layer, one face): pixelWidth {}, "
                                "pixelDepth {}, layerCount {}, faceCount {}",
                                w, depth, layers, faces));
    }
    const uint32_t maxDim = std::max(w, h);
    uint32_t possible = 1; // 1 + floor(log2(max(width, height)))
    while (possible < 32 && (maxDim >> possible) != 0) {
        ++possible;
    }
    const uint32_t levels = std::max(levelCount, 1u);
    if (levels > possible) {
        return fail(std::format("KTX2 levelCount {} is not possible for a {}x{} image", levelCount, w, h));
    }
    if (!inFile(kHeader, uint64_t(levels) * kLevelEntry, fileSize)) {
        return fail(std::format("KTX2 level index ({} levels) is outside the {}-byte file", levels, size));
    }
    const uint32_t dfdOffset = le32(data + 48), dfdLength = le32(data + 52);
    if (!inFile(dfdOffset, dfdLength, fileSize)) {
        return fail(std::format("KTX2 data format descriptor (byteOffset {}, byteLength {}) is outside the {}-byte file",
                                dfdOffset, dfdLength, size));
    }
    const uint32_t kvdOffset = le32(data + 56), kvdLength = le32(data + 60);
    if (!inFile(kvdOffset, kvdLength, fileSize)) {
        return fail(std::format("KTX2 key/value data (byteOffset {}, byteLength {}) is outside the {}-byte file",
                                kvdOffset, kvdLength, size));
    }
    const uint64_t sgdOffset = le64(data + 64), sgdLength = le64(data + 72);
    if (!inFile(sgdOffset, sgdLength, fileSize)) {
        return fail(std::format("KTX2 supercompression global data (byteOffset {}, byteLength {}) is outside the "
                                "{}-byte file",
                                sgdOffset, sgdLength, size));
    }
    // libktx reads the level data as one span from the last level's byteOffset (the smallest level is first in the
    // file) to the end of level 0, and indexes it by each level's byteOffset: every level must lie in that span.
    const uint8_t* index = data + kHeader;
    const uint64_t firstOffset = le64(index + (levels - 1) * kLevelEntry);
    const uint64_t level0End = le64(index) + le64(index + 8); // in the file once level 0 is checked below
    uint64_t inflated = 0;
    for (uint32_t l = 0; l < levels; ++l) {
        const uint8_t* e = index + l * kLevelEntry;
        const uint64_t offset = le64(e), length = le64(e + 8), uncompressed = le64(e + 16);
        if (!inFile(offset, length, fileSize)) {
            return fail(std::format("KTX2 level {} (byteOffset {}, byteLength {}) is outside the {}-byte file", l, offset,
                                    length, size));
        }
        if (offset < firstOffset || offset + length > level0End) {
            return fail(std::format("KTX2 level {} (byteOffset {}, byteLength {}) is outside the level data "
                                    "[{}, {}) (levels must be stored smallest first)",
                                    l, offset, length, firstOffset, level0End));
        }
        const uint64_t lw = std::max(1u, w >> l), lh = std::max(1u, std::max(h, 1u) >> l);
        const uint64_t blocks = ((lw + 3) / 4) * ((lh + 3) / 4); // <= 2^60
        const uint64_t uastc = blocks > (UINT64_MAX >> 4) ? UINT64_MAX : blocks * 16;
        if (uncompressed > uastc) {
            return fail(std::format("KTX2 level {} uncompressedByteLength {} exceeds the {} bytes a {}x{} UASTC level "
                                    "has",
                                    l, uncompressed, uastc, lw, lh));
        }
        if (uncompressed > maxBytes) {
            return fail(std::format("KTX2 level {} uncompressedByteLength {} exceeds r.Asset.MaxFileBytes ({})", l,
                                    uncompressed, maxBytes));
        }
        // + libktx's per-level alignment padding (at most 16 bytes); saturating, so the sum cannot wrap.
        const uint64_t padded = uncompressed > UINT64_MAX - 16 ? UINT64_MAX : uncompressed + 16;
        inflated = padded > UINT64_MAX - inflated ? UINT64_MAX : inflated + padded;
    }
    // KTX_SS_ZSTD = 2, KTX_SS_ZLIB = 3: libktx allocates the inflated levels in one buffer.
    if ((scheme == 2 || scheme == 3) && inflated > maxBytes) {
        return fail(std::format("KTX2 levels inflate to more than r.Asset.MaxFileBytes ({} bytes declared, cap {})",
                                inflated, maxBytes));
    }
    return {};
}

Result<void> create(const uint8_t* data, size_t size, const std::string& what, uint64_t maxBytes, Texture& out) {
    if (auto r = checkLayout(data, size, what, maxBytes); !r) {
        return r;
    }
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

Result<TranscodedImage> transcode(const uint8_t* data, size_t size, const std::string& what, uint64_t maxBytes,
                                  TranscodeTarget target, uint32_t baseLevel) {
    Texture tex;
    if (auto r = create(data, size, what, maxBytes, tex); !r) {
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
    // Image::encoded passed inspectKtx2 with the load's r.Asset.MaxFileBytes and the decode budget; both are checked
    // again here (the default cap: an image within the decode budget inflates to far less).
    const auto* data = reinterpret_cast<const uint8_t*>(image.encoded.data());
    if (image.encoded.size() >= 28 && uint64_t(le32(data + 20)) * le32(data + 24) > detail::kMaxDecodePixels) {
        return std::unexpected(Error{kExit, std::format("transcodeKtx2: '{}' {}x{} exceeds the decode budget ({} "
                                                        "pixels)",
                                                        image.name, le32(data + 20), le32(data + 24),
                                                        detail::kMaxDecodePixels)});
    }
    return transcode(image.encoded.data(), image.encoded.size(), image.name.empty() ? image.uri : image.name,
                     LoadOptions{}.maxFileBytes, target, image.ktx2BaseLevel);
}

namespace detail {

Result<void> inspectKtx2(const Context& c, std::span<const std::byte> bytes, const std::string& what, Image& img) {
    const auto* data = reinterpret_cast<const uint8_t*>(bytes.data());
    // Layout first, then the decode budget on the declared size, and only then libktx: the uncompressedByteLength
    // cap of checkLayout follows the declared size, so an over-budget 46336 x 46336 file could still declare 2 GiB.
    if (auto r = checkLayout(data, bytes.size(), what, c.options.maxFileBytes); !r) {
        return std::unexpected(r.error());
    }
    if (overDecodeBudget(c, what, le32(data + 20), le32(data + 24))) {
        return {};
    }
    uint32_t w = 0, h = 0, levels = 0;
    bool srgb = false;
    {
        Texture tex;
        if (auto r = create(data, bytes.size(), what, c.options.maxFileBytes, tex); !r) {
            return std::unexpected(r.error());
        }
        ktxTexture* base = ktxTexture(tex.t);
        w = base->baseWidth;
        h = base->baseHeight;
        levels = base->numLevels;
        srgb = ktxTexture2_GetTransferFunction_e(tex.t) == KHR_DF_TRANSFER_SRGB;
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
    auto t = transcode(data, bytes.size(), what, c.options.maxFileBytes, TranscodeTarget::RGBA8, 0);
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
