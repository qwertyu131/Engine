cmake_minimum_required(VERSION 3.28)

project(Engine
    VERSION 0.0.1
    DESCRIPTION "Cross-platform native glTF 2.0 renderer"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ENG_BUILD_TESTS "Build unit tests" ON)
# shaderc is an offline Windows build tool (DXC/FXC/Tint toolchain of plan F1); other hosts skip it.
if(WIN32)
    option(ENG_BUILD_SHADERC "Build the offline shader compiler tools/shaderc" ON)
else()
    option(ENG_BUILD_SHADERC "Build the offline shader compiler tools/shaderc" OFF)
endif()

include(cmake/Dependencies.cmake)
if(ENG_BUILD_SHADERC)
    include(cmake/ShaderTools.cmake)
endif()

add_subdirectory(engine)
add_subdirectory(tools/golden)
if(ENG_BUILD_SHADERC)
    add_subdirectory(tools/shaderc)

    # Shader build step (plan F2 Task 2): every shader of shaders/library.json for the RHI targets,
    # into ${ENG_SHADER_LIB_DIR}/<shader>/<target>/<key>/ (engine/rhi ShaderLibrary). The stamp is
    # written only after compile_shaders.py exits 0, so a failed compile reruns on the next build.
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(ENG_SHADER_LIB_DIR "${CMAKE_BINARY_DIR}/shaders")
    file(GLOB_RECURSE ENG_SHADER_INPUTS CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/shaders/*.hlsl" "${PROJECT_SOURCE_DIR}/shaders/*.hlsli" "${PROJECT_SOURCE_DIR}/shaders/*.json")
    add_custom_command(
        OUTPUT "${ENG_SHADER_LIB_DIR}/.stamp"
        COMMAND "${CMAKE_COMMAND}" -E rm -f "${ENG_SHADER_LIB_DIR}/.stamp"
        COMMAND Python3::Interpreter "${PROJECT_SOURCE_DIR}/scripts/compile_shaders.py"
                --library "${PROJECT_SOURCE_DIR}/shaders/library.json" --targets spirv,dxbc
                --out "${ENG_SHADER_LIB_DIR}" --shaderc "$<TARGET_FILE:shaderc>"
        COMMAND "${CMAKE_COMMAND}" -E touch "${ENG_SHADER_LIB_DIR}/.stamp"
        DEPENDS shaderc ${ENG_SHADER_INPUTS} "${PROJECT_SOURCE_DIR}/scripts/compile_shaders.py"
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        COMMENT "Compiling RHI shaders (shaders/library.json -> ${ENG_SHADER_LIB_DIR})"
        VERBATIM)
    add_custom_target(shaders ALL DEPENDS "${ENG_SHADER_LIB_DIR}/.stamp")
endif()
add_subdirectory(apps/headless)
if(WIN32)
    add_subdirectory(apps/viewer)
endif()

# D3D11 VS -> PS linkage checker (plan F2 Task 1; spike tool, not run by ctest). Windows-only, optional.
if(WIN32)
    option(ENG_BUILD_D3D11_LINKCHECK "Build tests/tools/d3d11_linkcheck" ON)
    if(ENG_BUILD_D3D11_LINKCHECK)
        add_subdirectory(tests/tools/d3d11_linkcheck)
    endif()
endif()

if(ENG_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests/unit)
    add_subdirectory(tests/golden)
    if(WIN32)
        add_subdirectory(tests/viewer)
    endif()
endif()
