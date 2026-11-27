#pragma once
// Loose shaderc outputs as written by the `shaders` build step (scripts/compile_shaders.py --library,
// plan F2 Task 2); the packed .shaderpack is plan F5.
//
//   <dir>/<shader>/<target>/<key>/<shader>.<entry>.<target>.<ext>      (spirv: .spv, dxbc: .dxbc)
//   <dir>/<shader>/<target>/<key>/<shader>.<entry>.<target>.refl.json
//
// target = spirv (Backend::Vulkan) or dxbc (Backend::D3D11); key = permutation key, e.g.
// T2_SKIN0_MORPH0_UNLIT0, or "default" for a shader without permutations. Paths are UTF-8 safe.
#include "Types.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace eng::rhi {

class ShaderLibrary {
public:
    // Error{2, "shader not found: <path>"} if the blob or its reflection is missing;
    // Error{2, "invalid shader reflection <path>: ..."} if the JSON does not match the schema.
    static Result<ShaderBlob> load(const std::filesystem::path& dir, std::string_view shader, std::string_view entry,
                                   Backend backend, std::string_view key);

    static std::string targetName(Backend backend); // spirv | dxbc
    static std::filesystem::path blobPath(const std::filesystem::path& dir, std::string_view shader,
                                          std::string_view entry, Backend backend, std::string_view key);
};

// UTF-8 <-> path (never path::string(): the Windows ANSI codepage mangles non-ASCII paths).
std::filesystem::path pathFromUtf8(std::string_view utf8);
std::string pathToUtf8(const std::filesystem::path& p);

} // namespace eng::rhi
