#pragma once
// glTF 2.0 loader (plan F3 Task 2): .gltf (external / data-URI buffers and images) and .glb -> ModelData.
//
// Errors (Result error, exit code 2, never a crash, hang or out-of-bounds read; Review Focus 1):
//   - unreadable / truncated / malformed files, a bad GLB container, invalid JSON;
//   - an accessor, buffer view, sparse index or vertex index out of range; counts and offsets are checked with
//     overflow-safe 64-bit arithmetic before anything is allocated;
//   - a data-URI that is not valid base64, an image that cannot be decoded;
//   - spec §7.2: an unknown extension in extensionsRequired -> "unsupported required extension <name>"; a known
//     later-phase one -> "<name> is supported from F<n>".
// Diagnostics (Warning / Info, Extensions.cpp): an unknown extension in extensionsUsed is a warning and ignored;
// a known later-phase one is parsed and ignored with an info message; caps exceeded (MaxTextureSize downscales,
// MaxVertices drops the primitive) are warnings.
#include "ModelData.h"
#include "core/Result.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace eng::asset {

struct LoadOptions {
    uint32_t maxTextureSize = 8192;      // r.Asset.MaxTextureSize: larger images are downscaled (warning)
    uint64_t maxVertices = 16u << 20;    // r.Asset.MaxVertices: a larger primitive is dropped (warning)
    uint64_t maxFileBytes = 2ull << 30;  // r.Asset.MaxFileBytes: a larger file (.gltf / .glb / .bin / image) is refused
    bool generateTangents = true;        // MikkTSpace for primitives with a normal map and no TANGENT
};

struct Diagnostic {
    enum Level { Info, Warning } level;
    std::string message;
};

// `diags` may be null. Errors have exitCode 2.
Result<ModelData> loadGltf(const std::filesystem::path& path, const LoadOptions& options, std::vector<Diagnostic>* diags);

// r.Asset.MaxTextureSize / r.Asset.MaxVertices / r.Asset.MaxFileBytes (registered on first use, CVarFlags::None).
// MaxFileBytes is in MiB (an int cvar). The facade (Task 4) passes this to loadGltf.
LoadOptions loadOptionsFromCVars();

} // namespace eng::asset
