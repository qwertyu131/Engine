// ShaderLibrary (plan F2 Task 2): loose shaderc outputs from the `shaders` build step.
//   - triangle.VSMain (the F1 test shader) loads for both backends; every reflection field equals the
//     .refl.json shaderc wrote (F1 schema);
//   - a missing file is Error{2, "shader not found: <path>"};
//   - a shader directory with non-ASCII characters and spaces works.
#include "rhi/ShaderLibrary.h"

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>

#include "RhiTestEnv.h"

namespace fs = std::filesystem;
using namespace eng::rhi;

namespace {

nlohmann::json readJson(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), pathToUtf8(p));
    return nlohmann::json::parse(in);
}

void checkAgainstJson(const ShaderBlob& blob, const fs::path& jsonPath, Backend backend) {
    const nlohmann::json j = readJson(jsonPath);
    const ShaderReflection& r = blob.reflection;
    CHECK(r.entry == j.at("entry").get<std::string>());
    CHECK(r.target == j.at("target").get<std::string>());
    CHECK((j.at("stage") == "vs") == (r.stage == ShaderStage::Vertex));
    REQUIRE(r.resources.size() == j.at("resources").size());
    for (size_t i = 0; i < r.resources.size(); ++i) {
        const auto& jr = j.at("resources")[i];
        const ReflectedResource& x = r.resources[i];
        CAPTURE(x.name);
        CHECK(x.name == jr.at("name").get<std::string>());
        CHECK(x.kind == jr.at("kind").get<std::string>());
        CHECK(x.space == jr.at("space").get<uint32_t>());
        CHECK(x.index == jr.at("index").get<uint32_t>());
        CHECK(x.arraySize == jr.at("arraySize").get<uint32_t>());
        CHECK(x.sizeBytes == jr.value("sizeBytes", 0u));
        const auto& tb = jr.at("targetBinding");
        if (backend == Backend::Vulkan) {
            CHECK(x.set == tb.at("set").get<uint32_t>());
            CHECK(x.binding == tb.at("binding").get<uint32_t>());
        } else {
            const std::string reg = tb.at("register").get<std::string>();
            CHECK(std::string(1, x.registerClass) + std::to_string(x.registerSlot) == reg);
        }
    }
    REQUIRE(r.vertexInputs.size() == j.at("vertexInputs").size());
    for (size_t i = 0; i < r.vertexInputs.size(); ++i) {
        const auto& jv = j.at("vertexInputs")[i];
        CHECK(r.vertexInputs[i].location == jv.at("location").get<uint32_t>());
        CHECK(r.vertexInputs[i].semantic == jv.at("semantic").get<std::string>());
        CHECK(r.vertexInputs[i].format == jv.at("format").get<std::string>());
    }
}

} // namespace

TEST_SUITE("rhi.shaderlib") {

TEST_CASE("triangle.VSMain loads for both backends; reflection fields equal the F1 JSON") {
    const fs::path dir = eng::rhi::test::shaderLibDir();
    for (Backend b : {Backend::Vulkan, Backend::D3D11}) {
        CAPTURE(toString(b));
        auto blob = ShaderLibrary::load(dir, "triangle", "VSMain", b, "default");
        REQUIRE_MESSAGE(blob.has_value(), (blob ? std::string() : blob.error().message));
        CHECK((blob->backend == b));
        CHECK((blob->stage == ShaderStage::Vertex));
        CHECK(blob->entry == "VSMain");
        REQUIRE(blob->code.size() > 32);
        if (b == Backend::Vulkan) {
            CHECK(blob->code[0] == 0x03); // SPIR-V magic 0x07230203, little endian
            CHECK(blob->code[3] == 0x07);
        } else {
            CHECK(std::string(blob->code.begin(), blob->code.begin() + 4) == "DXBC");
        }
        fs::path json = ShaderLibrary::blobPath(dir, "triangle", "VSMain", b, "default");
        json.replace_extension(".refl.json");
        checkAgainstJson(*blob, json, b);
        // The F1 triangle itself: Frame (b0, space0) and POSITION / TEXCOORD0 / COLOR0.
        REQUIRE(blob->reflection.resources.size() == 1);
        CHECK(blob->reflection.resources[0].name == "Frame");
        CHECK(blob->reflection.resources[0].kind == "cbuffer");
        CHECK(blob->reflection.resources[0].sizeBytes == 64);
        REQUIRE(blob->reflection.vertexInputs.size() == 3);
        CHECK(blob->reflection.vertexInputs[0].semantic == "POSITION");
        CHECK(blob->reflection.vertexInputs[1].semantic == "TEXCOORD0");
        CHECK(blob->reflection.vertexInputs[2].semantic == "COLOR0");
    }
}

TEST_CASE("a missing shader is Error{2, shader not found: <path>}") {
    const fs::path dir = eng::rhi::test::shaderLibDir();
    auto r = ShaderLibrary::load(dir, "no_such_shader", "VSMain", Backend::Vulkan, "default");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().exitCode == 2);
    CHECK(r.error().message ==
          "shader not found: " + pathToUtf8(ShaderLibrary::blobPath(dir, "no_such_shader", "VSMain", Backend::Vulkan,
                                                                    "default")));
}

TEST_CASE("shader directory under a non-ASCII path with spaces") {
    const fs::path src = eng::rhi::test::shaderLibDir() / "triangle";
    const fs::path dir = fs::temp_directory_path() / u8"eng-rhi-shaderlib" / u8"ç ğ ü shaders";
    fs::remove_all(dir.parent_path());
    fs::create_directories(dir);
    fs::copy(src, dir / "triangle", fs::copy_options::recursive);
    for (Backend b : {Backend::Vulkan, Backend::D3D11}) {
        auto blob = ShaderLibrary::load(dir, "triangle", "PSMain", b, "default");
        REQUIRE_MESSAGE(blob.has_value(), (blob ? std::string() : blob.error().message));
        CHECK((blob->stage == ShaderStage::Fragment));
    }
    fs::remove_all(dir.parent_path());
}

} // TEST_SUITE
