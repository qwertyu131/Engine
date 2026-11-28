#pragma once
// KHR_texture_basisu transcoding (plan F3 Task 3). The loader keeps a KTX2 image compressed in ModelData (Image::ktx2,
// Image::encoded); the renderer transcodes it at upload to a format the device samples. engine/asset has no GPU
// types: the device's support is passed as TranscodeSupport (the renderer fills it from rhi::DeviceCaps) and the
// result names its block format with TranscodeTarget (the renderer maps it to rhi::Format, sRGB from `srgb`).
//
// Target choice (plan F3 Task 3, the same for ETC1S and UASTC payloads):
//   BC7 if supported, else BC1 / BC3 (BC1 when the image has no alpha, 3-colour BC1 blocks forbidden so a BC1 texel
//   is always opaque), else ASTC 4x4, else ETC2 RGBA8 (the mobile paths of F11; unverified on this machine's desktop
//   GPU, which supports neither), else RGBA8. Block formats need a base level whose width and height are multiples of
//   4 (D3D11 requires it for BC; applied to every backend alike): otherwise RGBA8.
// The transfer function comes from the KTX2 DFD (Image::ktx2Srgb), never from the material slot.
#include "ModelData.h"
#include "core/Result.h"

#include <cstdint>
#include <vector>

namespace eng::asset {

enum class TranscodeTarget : uint8_t { RGBA8, BC1, BC3, BC7, ETC2_RGBA8, ASTC_4x4 };
const char* toString(TranscodeTarget t);

struct TranscodeSupport {
    bool bc7 = false;       // BC7 (DeviceCaps::bc6h7 + BC7 sample / filter)
    bool bc1bc3 = false;    // BC1 + BC3 (DeviceCaps::bc)
    bool astc = false;      // ASTC 4x4 LDR
    bool etc2 = false;      // ETC2 RGBA8
};

// The target for `image` (Image::ktx2) on a device with `support`.
TranscodeTarget chooseTranscodeTarget(const TranscodeSupport& support, const Image& image);

struct TranscodedLevel {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> data; // block rows (4 x 4 blocks) or RGBA8 rows, top-down, tightly packed
};

struct TranscodedImage {
    TranscodeTarget target = TranscodeTarget::RGBA8; // BC1 / BC3 resolved to the one used
    bool srgb = false;                               // the DFD transfer function
    std::vector<TranscodedLevel> levels;             // Image::ktx2BaseLevel and smaller
};

// Transcodes a KTX2 image (Image::ktx2) to `target` (BC1 and BC3 are interchangeable: the image's alpha decides).
// Error{2} when the payload does not transcode.
Result<TranscodedImage> transcodeKtx2(const Image& image, TranscodeTarget target);

} // namespace eng::asset
