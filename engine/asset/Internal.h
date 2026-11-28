#pragma once
// engine/asset internals (plan F3 Task 2): the load context shared by GltfLoader.cpp, Accessors.cpp,
// Primitives.cpp, Images.cpp. Not part of the public interface.
#include "GltfLoader.h"

#include <fastgltf/core.hpp>
#include <fastgltf/types.hpp>
#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace eng::asset::detail {

inline constexpr int kExitMalformed = 2;

inline Error fail(std::string message) {
    return Error{kExitMalformed, std::move(message)};
}

// a * b + c without wrapping; false on overflow.
inline bool mulAdd(uint64_t a, uint64_t b, uint64_t c, uint64_t& out) {
    if (b != 0 && a > (UINT64_MAX - c) / b) {
        return false;
    }
    out = a * b + c;
    return true;
}

struct Context {
    const fastgltf::Asset& asset;
    const nlohmann::json& json; // the same document, for raw extension objects and URIs
    const LoadOptions& options;
    std::vector<Diagnostic>* diags;
    std::filesystem::path baseDir; // directory of the .gltf / .glb (relative URIs)
    // Per glTF buffer: its bytes, exactly byteLength long (empty for an EXT_meshopt fallback buffer).
    std::vector<std::span<const std::byte>> buffers;
    std::vector<std::vector<std::byte>> owned; // external .bin storage behind `buffers`
    // Per bufferView (plan F3 Task 3, Meshopt.cpp): the decoded bytes of an EXT_meshopt_compression /
    // KHR_meshopt_compression view (byteLength long); bufferViewBytes() returns them instead of the buffer range.
    std::vector<std::vector<std::byte>> meshoptViews;
    std::vector<uint8_t> meshoptDecoded; // 1 = meshoptViews[i] holds view i
    // Per accessor (plan F3 Task 3, Draco.cpp): the decoded elements of a KHR_draco_mesh_compression accessor, in the
    // accessor's own componentType / type, tightly packed, exactly count elements. checkAccessor() reads them
    // instead of a bufferView. Filled by buildPrimitive before any accessor of the primitive is read.
    mutable std::unordered_map<size_t, std::vector<std::byte>> accessorData;

    void warn(std::string m) const {
        if (diags) {
            diags->push_back({Diagnostic::Warning, std::move(m)});
        }
    }
    void info(std::string m) const {
        if (diags) {
            diags->push_back({Diagnostic::Info, std::move(m)});
        }
    }
};

// Reads a whole file with the r.Asset.MaxFileBytes cap. `what` names it in errors ("buffer 0", "image 2").
Result<std::vector<std::byte>> readFileCapped(const std::filesystem::path& p, uint64_t cap, const std::string& what);

// A relative glTF URI (percent-encoded or raw UTF-8) -> path under baseDir. Error for remote / absolute URIs.
Result<std::filesystem::path> resolveUri(const std::filesystem::path& baseDir, const std::string& uri,
                                         const std::string& what);

// --- Accessors.cpp ---------------------------------------------------------------------------------------
// Every accessor read is bounds-checked against its bufferView and buffer (overflow-safe 64-bit math) and sparse
// substitution is applied. Errors name the accessor: "accessor <i>: ...".
uint32_t componentCount(fastgltf::AccessorType t); // 0 for Invalid
// count * components floats; normalized integers decoded per glTF 2.0 §3.11, other integers converted.
Result<std::vector<float>> readFloats(const Context& c, size_t accessor, uint32_t& components);
// count * components unsigned integers (UNSIGNED_BYTE / SHORT / INT only), e.g. indices, JOINTS_n.
Result<std::vector<uint32_t>> readUints(const Context& c, size_t accessor, uint32_t& components);
// The range checks of readFloats / readUints (bufferView, buffer, sparse) without decoding.
Result<void> validateAccessor(const Context& c, size_t accessor);
// The bytes of a buffer view, range-checked against its buffer.
Result<std::span<const std::byte>> bufferViewBytes(const Context& c, size_t view, const std::string& what);

// --- Primitives.cpp --------------------------------------------------------------------------------------
// The primitive or nothing (dropped with a warning: no POSITION, r.Asset.MaxVertices exceeded).
Result<bool> buildPrimitive(const Context& c, const ModelData& model, size_t mesh, size_t index,
                            const fastgltf::Primitive& in, Primitive& out);

// --- Images.cpp ------------------------------------------------------------------------------------------
// model.images from the asset's images; materials must be converted first (colour space by usage).
Result<void> loadImages(const Context& c, ModelData& model);
// Decoded pixel budget: larger images are refused (warning, image left undecoded) instead of allocating gigabytes.
inline constexpr uint64_t kMaxDecodePixels = uint64_t{16384} * 16384;
bool overDecodeBudget(const Context& c, const std::string& what, uint64_t w, uint64_t h); // warns when true
// r.Asset.MaxTextureSize for a decoded RGBA8 image: halved (2 x 2 box, linear light for sRGB) until it fits, warning.
void fitTextureSize(const Context& c, const std::string& what, Image& img);

// --- Meshopt.cpp (plan F3 Task 3) ------------------------------------------------------------------------
// Decodes every EXT_meshopt_compression / KHR_meshopt_compression buffer view into c.meshoptViews (after the
// buffers are set up, before any accessor is read). Errors name the view: "bufferView <i> (meshopt): ...".
Result<void> decodeMeshoptViews(Context& c);

// --- Draco.cpp (plan F3 Task 3) --------------------------------------------------------------------------
// KHR_draco_mesh_compression: decodes the primitive's Draco bufferView and stores every mapped attribute accessor and
// the indices accessor in c.accessorData. No-op for a primitive without the extension.
Result<void> decodeDracoPrimitive(const Context& c, const fastgltf::Primitive& in, const std::string& where);

// --- Webp.cpp / Ktx2.cpp (plan F3 Task 3) ----------------------------------------------------------------
// EXT_texture_webp: decoded to RGBA8 (budget, r.Asset.MaxTextureSize as PNG / JPEG). Error = exit 2.
Result<void> decodeWebp(const Context& c, std::span<const std::byte> bytes, const std::string& what, Image& img);
// KHR_texture_basisu: the KTX2 file is parsed and validated (2D, Basis Universal ETC1S / UASTC payload), kept in
// img.encoded with its levels and DFD transfer function; r.Asset.MaxTextureSize skips the larger mip levels, or,
// without enough levels, transcodes to RGBA8 and halves. Error = exit 2.
Result<void> inspectKtx2(const Context& c, std::span<const std::byte> bytes, const std::string& what, Image& img);

} // namespace eng::asset::detail
