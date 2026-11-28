#pragma once
// glTF image -> GPU texture (plan F3 Task 3; used by GpuModel from Task 4). One place decides the GPU format of every
// glTF image, so the renderer and the Review Focus 3 tests share it:
//   decoded RGBA8 (PNG / JPEG / WebP)  -> RGBA8_SRGB when the view is sRGB, else RGBA8_UNORM; a full mip chain is
//                                         generated on the CPU (2 x 2 box filter, in linear light for sRGB views);
//   KTX2 (KHR_texture_basisu)          -> transcoded to the target asset::chooseTranscodeTarget picks from the
//                                         device's caps (BC7 > BC1 / BC3 > ASTC > ETC2 > RGBA8), *_SRGB exactly when the
//                                         KTX2 DFD transfer function is sRGB; the file's own mip levels.
// The sRGB view of a decoded image comes from its use (Review Focus 3: baseColor / emissive sRGB, metallicRoughness /
// normal / occlusion linear). An image used both ways gets one texture per view (the caller asks for each).
#include "asset/Ktx2.h"
#include "asset/ModelData.h"
#include "rhi/Rhi.h"

#include <cstdint>
#include <string>
#include <vector>

namespace eng::renderer {

// What the device samples, for the KTX2 transcode target (DeviceCaps bc / bc6h7 / etc2 / astc + per-format sample /
// filter support of the sRGB and UNORM variants).
asset::TranscodeSupport transcodeSupport(const rhi::DeviceCaps& caps);

// rhi::Format of a transcode target.
rhi::Format gpuFormat(asset::TranscodeTarget target, bool srgb);

// CPU-side texture: the desc (format, size, mips) and the bytes of every mip level (texel rows or 4 x 4 block rows).
struct TextureData {
    rhi::TextureDesc desc;
    std::vector<std::vector<uint8_t>> levels;
};

// The GPU-ready data of `image` for a view in `srgbView` colour space (ignored for KTX2: its DFD decides). Error{2}
// for an image without data (over the decode budget) or a KTX2 that does not transcode.
Result<TextureData> prepareTexture(const asset::Image& image, const rhi::DeviceCaps& caps, bool srgbView);

// Creates the texture (usage Sampled, every level uploaded).
Result<rhi::TextureHandle> createTexture(rhi::IDevice& device, const TextureData& data);

} // namespace eng::renderer
