# Offline shader compiler (plan F1 Task 2): library `shaderc_core` + CLI `shaderc`.
# HLSL -> DXC -> DXIL / SPIR-V; SPIR-V -> SPIRV-Cross (library) -> MSL / GLSL ES / HLSL SM5 (-> FXC -> DXBC);
# SPIR-V -> Tint -> WGSL. Windows-only (CreateProcessW, FXC); see ENG_BUILD_SHADERC.
#
# C++23 for these targets only (std::expected, as tools/golden); the `engine` library stays C++20.
add_library(shaderc_core STATIC
    Targets.h
    Targets.cpp
    Process.h
    Process.cpp
    Compiler.h
    Compiler.cpp
    Reflection.h
    Reflection.cpp
    Validate.h
    Validate.cpp)
target_include_directories(shaderc_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(shaderc_core PUBLIC cxx_std_23)
target_link_libraries(shaderc_core
    PUBLIC  nlohmann_json::nlohmann_json
    PRIVATE spirv-cross-core spirv-cross-glsl spirv-cross-msl spirv-cross-hlsl)

add_executable(shaderc main.cpp)
target_link_libraries(shaderc PRIVATE shaderc_core)
# Compiled-in defaults for the tool paths (from cmake/ShaderTools.cmake); `--tool name=path` overrides.
target_compile_definitions(shaderc PRIVATE
    ENG_DEFAULT_DXC="${ENG_DXC}"
    ENG_DEFAULT_FXC="${ENG_FXC}"
    ENG_DEFAULT_TINT="${ENG_TINT}"
    ENG_DEFAULT_SPIRV_VAL="${ENG_SPIRV_VAL}"
    ENG_DEFAULT_GLSLANG="${ENG_GLSLANG}")

foreach(t shaderc_core shaderc)
    if(MSVC)
        target_compile_options(${t} PRIVATE /W4 /WX /permissive- /utf-8 /external:W0)
    else()
        target_compile_options(${t} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endforeach()
