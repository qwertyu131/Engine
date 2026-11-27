#include "ShaderLibrary.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <iterator>
#include <system_error>

namespace fs = std::filesystem;

namespace eng::rhi {

fs::path pathFromUtf8(std::string_view utf8) {
    return fs::path(std::u8string(utf8.begin(), utf8.end()));
}

std::string pathToUtf8(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::string ShaderLibrary::targetName(Backend backend) {
    return backend == Backend::Vulkan ? "spirv" : "dxbc";
}

fs::path ShaderLibrary::blobPath(const fs::path& dir, std::string_view shader, std::string_view entry, Backend backend,
                                 std::string_view key) {
    const std::string target = targetName(backend);
    const std::string ext = backend == Backend::Vulkan ? "spv" : "dxbc";
    return dir / pathFromUtf8(shader) / pathFromUtf8(target) / pathFromUtf8(key) /
           pathFromUtf8(std::string(shader) + "." + std::string(entry) + "." + target + "." + ext);
}

namespace {

std::expected<std::vector<uint8_t>, Error> readAll(const fs::path& p) {
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) {
        return std::unexpected(Error{kExitUsage, "shader not found: " + pathToUtf8(p)});
    }
    std::ifstream in(p, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) {
        return std::unexpected(Error{kExitUsage, "cannot read shader " + pathToUtf8(p)});
    }
    return bytes;
}

ShaderStage stageOf(const std::string& s) {
    return s == "vs" ? ShaderStage::Vertex : s == "fs" ? ShaderStage::Fragment : ShaderStage::Compute;
}

} // namespace

Result<ShaderBlob> ShaderLibrary::load(const fs::path& dir, std::string_view shader, std::string_view entry,
                                       Backend backend, std::string_view key) {
    const fs::path blob = blobPath(dir, shader, entry, backend, key);
    fs::path reflPath = blob;
    reflPath.replace_extension(".refl.json");
    auto code = readAll(blob);
    if (!code) {
        return std::unexpected(code.error());
    }
    auto reflBytes = readAll(reflPath);
    if (!reflBytes) {
        return std::unexpected(reflBytes.error());
    }
    ShaderBlob out;
    out.backend = backend;
    out.entry = std::string(entry);
    out.code = std::move(*code);
    try {
        const nlohmann::json j = nlohmann::json::parse(reflBytes->begin(), reflBytes->end());
        ShaderReflection& r = out.reflection;
        r.stage = stageOf(j.at("stage").get<std::string>());
        r.entry = j.at("entry").get<std::string>();
        r.target = j.at("target").get<std::string>();
        r.dxbcPath = j.value("dxbcPath", std::string());
        r.renderTargets = j.value("renderTargets", 0u);
        for (const auto& jr : j.at("resources")) {
            ReflectedResource x;
            x.name = jr.at("name").get<std::string>();
            x.kind = jr.at("kind").get<std::string>();
            x.space = jr.at("space").get<uint32_t>();
            x.index = jr.at("index").get<uint32_t>();
            x.arraySize = jr.at("arraySize").get<uint32_t>();
            x.sizeBytes = jr.value("sizeBytes", 0u);
            const auto& tb = jr.at("targetBinding");
            if (tb.contains("set")) {
                x.set = tb.at("set").get<uint32_t>();
                x.binding = tb.at("binding").get<uint32_t>();
            }
            if (tb.contains("register")) {
                const std::string reg = tb.at("register").get<std::string>();
                if (reg.size() < 2) {
                    throw std::runtime_error("bad register '" + reg + "'");
                }
                x.registerClass = reg[0];
                x.registerSlot = static_cast<uint32_t>(std::stoul(reg.substr(1)));
            }
            r.resources.push_back(std::move(x));
        }
        for (const auto& jv : j.at("vertexInputs")) {
            r.vertexInputs.push_back({jv.at("location").get<uint32_t>(), jv.at("semantic").get<std::string>(),
                                      jv.at("format").get<std::string>()});
        }
        out.stage = r.stage;
        if (r.entry != entry) {
            throw std::runtime_error("entry is '" + r.entry + "'");
        }
    } catch (const std::exception& e) {
        return std::unexpected(Error{kExitUsage, "invalid shader reflection " + pathToUtf8(reflPath) + ": " + e.what()});
    }
    return out;
}

} // namespace eng::rhi
