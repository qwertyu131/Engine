// shaderc: one trivial shader (shaders/test/triangle.hlsl) through all 7 targets (plan F1 Task 2).
#include "Compiler.h"
#include "Targets.h"
#include "Validate.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "ShaderTestTools.h"

namespace fs = std::filesystem;
using namespace eng::sc;

namespace {

const std::vector<std::pair<std::string, Stage>> kEntries = {{"VSMain", Stage::Vertex}, {"PSMain", Stage::Fragment}};

CompileRequest request(const fs::path& src, const std::string& entry, Stage stage, Target target) {
    CompileRequest req;
    req.src = src;
    req.entry = entry;
    req.stage = stage;
    req.target = target;
    return req;
}

void compileAndValidate(const fs::path& src, Target target, DxbcPath dxbcPath = DxbcPath::Cross) {
    for (const auto& [entry, stage] : kEntries) {
        CAPTURE(toString(target));
        CAPTURE(entry);
        CompileRequest req = request(src, entry, stage, target);
        req.dxbcPath = dxbcPath;
        auto r = compile(req, test::tools());
        REQUIRE_MESSAGE(r.has_value(), (r.has_value() ? std::string() : r.error()));
        CHECK((!r->bytecode.empty() || !r->text.empty()));
        if (isTextTarget(target)) {
            CHECK(!r->text.empty());
        } else {
            CHECK(!r->bytecode.empty());
        }
        CHECK(r->reflection.at("target") == toString(target));
        CHECK(r->reflection.at("entry") == entry);
        CHECK(r->reflection.at("stage") == toString(stage));

        auto v = validate(target, *r, test::tools());
        REQUIRE_MESSAGE(v.has_value(), (v.has_value() ? std::string() : v.error()));
        CHECK(*v == (target == Target::MSL ? ValidateStatus::PendingCI : ValidateStatus::Passed));
    }
}

} // namespace

TEST_SUITE("shaderc") {

TEST_CASE("triangle VS+FS compiles and validates for every target") {
    for (Target t : kAllTargets) {
        compileAndValidate(test::triangleSrc(), t);
    }
}

TEST_CASE("DXBC: both the direct FXC path and the SPIRV-Cross path work") {
    compileAndValidate(test::triangleSrc(), Target::DXBC, DxbcPath::Direct);
    compileAndValidate(test::triangleSrc(), Target::DXBC, DxbcPath::Cross);
}

TEST_CASE("source and output under a non-ASCII path with spaces") {
    const fs::path dir = fs::temp_directory_path() / u8"eng-shaderc-test" / u8"ç ğ ü test";
    fs::remove_all(dir.parent_path());
    fs::create_directories(dir);
    const fs::path src = dir / u8"triangle.hlsl";
    fs::copy_file(test::triangleSrc(), src);
    compileAndValidate(src, Target::DXBC, DxbcPath::Direct); // FXC sees the DXC-preprocessed copy
    for (Target t : kAllTargets) {
        compileAndValidate(src, t);
        auto r = compile(request(src, "PSMain", Stage::Fragment, t), test::tools());
        REQUIRE(r.has_value());
        auto written = writeOutputs(*r, dir / u8"out ç", src, "PSMain", t);
        REQUIRE_MESSAGE(written.has_value(), (written.has_value() ? std::string() : written.error()));
        const std::string base = "triangle.PSMain." + toString(t);
        const fs::path out = dir / u8"out ç" / test::u8path(base + "." + extension(t));
        CHECK(fs::exists(out));
        CHECK(fs::file_size(out) > 0);
        const fs::path refl = dir / u8"out ç" / test::u8path(base + ".refl.json");
        CHECK(fs::exists(refl));
    }
    fs::remove_all(dir.parent_path());
}

TEST_CASE("a missing tool is reported by name") {
    ToolPaths tools = test::tools();
    tools.dxc = fs::path(u8"C:/no/such/dir/dxc.exe");
    auto r = compile(request(test::triangleSrc(), "VSMain", Stage::Vertex, Target::SPIRV), tools);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error() == "tool not found: dxc (run scripts/fetch_shader_tools.py)");

    tools = test::tools();
    tools.spirvVal.clear();
    auto ok = compile(request(test::triangleSrc(), "VSMain", Stage::Vertex, Target::SPIRV), tools);
    REQUIRE(ok.has_value());
    auto v = validate(Target::SPIRV, *ok, tools);
    REQUIRE_FALSE(v.has_value());
    CHECK(v.error() == "tool not found: spirv-val (run scripts/fetch_shader_tools.py)");
}

TEST_CASE("a compile error is reported, not an empty output") {
    const fs::path dir = fs::temp_directory_path() / u8"eng-shaderc-bad";
    fs::create_directories(dir);
    const fs::path src = dir / "bad.hlsl";
    std::ofstream(src) << "float4 VSMain() : SV_Position { return undefined_symbol; }\n";
    auto r = compile(request(src, "VSMain", Stage::Vertex, Target::SPIRV), test::tools());
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().find("undefined_symbol") != std::string::npos);
    fs::remove_all(dir);
}

TEST_CASE("each validator rejects a broken output") {
    const fs::path src = test::triangleSrc();
    auto check = [](Target t, const CompileResult& broken) {
        CAPTURE(toString(t));
        auto v = validate(t, broken, test::tools());
        CHECK_FALSE(v.has_value());
    };
    auto spirv = compile(request(src, "VSMain", Stage::Vertex, Target::SPIRV), test::tools());
    REQUIRE(spirv.has_value());
    CompileResult truncated = *spirv;
    truncated.bytecode.resize(truncated.bytecode.size() / 2);
    check(Target::SPIRV, truncated);

    for (Target t : {Target::GLSL_ES300, Target::GLSL_ES320}) {
        auto glsl = compile(request(src, "PSMain", Stage::Fragment, t), test::tools());
        REQUIRE(glsl.has_value());
        CompileResult broken = *glsl;
        broken.text += "\nthis is not glsl;\n";
        check(t, broken);
    }

    auto wgsl = compile(request(src, "PSMain", Stage::Fragment, Target::WGSL), test::tools());
    REQUIRE(wgsl.has_value());
    CompileResult brokenWgsl = *wgsl;
    brokenWgsl.text += "\nfn broken( {\n";
    check(Target::WGSL, brokenWgsl);

    auto dxbc = compile(request(src, "PSMain", Stage::Fragment, Target::DXBC), test::tools());
    REQUIRE(dxbc.has_value());
    CompileResult zeroDigest = *dxbc;
    REQUIRE(zeroDigest.bytecode.size() > 20);
    std::fill(zeroDigest.bytecode.begin() + 4, zeroDigest.bytecode.begin() + 20, uint8_t(0));
    check(Target::DXBC, zeroDigest);
}

TEST_CASE("DXIL validation needs dxil.dll next to dxc.exe") {
    // dxc.exe + dxcompiler.dll without dxil.dll, as `shaderc --tool dxc=<copy>` would see it.
    const fs::path dir = fs::temp_directory_path() / u8"eng-shaderc-nodxil" / u8"ç dxc";
    fs::remove_all(dir.parent_path());
    fs::create_directories(dir);
    const fs::path real = test::tools().dxc;
    fs::copy_file(real, dir / "dxc.exe");
    fs::copy_file(real.parent_path() / "dxcompiler.dll", dir / "dxcompiler.dll");
    ToolPaths tools = test::tools();
    tools.dxc = dir / "dxc.exe";
    auto ok = compile(request(test::triangleSrc(), "PSMain", Stage::Fragment, Target::DXIL), test::tools());
    REQUIRE(ok.has_value());
    auto v = validate(Target::DXIL, *ok, tools);
    REQUIRE_FALSE(v.has_value());
    CHECK(v.error().rfind("tool not found: dxil.dll (", 0) == 0);
    fs::remove_all(dir.parent_path());
}

} // TEST_SUITE
