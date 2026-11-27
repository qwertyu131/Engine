// shaderc unit tests. Every case needs the full offline toolchain; when a tool is missing the whole
// binary reports SKIP with the reason (exit 77 = ctest SKIP_RETURN_CODE), never a silent pass.
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <cstdio>
#include <filesystem>

#include "ShaderTestTools.h"

int main(int argc, char** argv) {
    const eng::sc::ToolPaths t = eng::sc::test::tools();
    const struct { const char* name; const std::filesystem::path& path; } required[] = {
        {"dxc", t.dxc}, {"fxc", t.fxc}, {"tint", t.tint}, {"spirv-val", t.spirvVal}, {"glslangValidator", t.glslang}};
    bool missing = false;
    for (const auto& r : required) {
        std::error_code ec;
        if (r.path.empty() || !std::filesystem::is_regular_file(r.path, ec)) {
            std::printf("SKIP: tool not found: %s (run scripts/fetch_shader_tools.py)\n", r.name);
            missing = true;
        }
    }
    if (missing) {
        return 77;
    }
    doctest::Context ctx(argc, argv);
    return ctx.run();
}
