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
endif()
add_subdirectory(apps/headless)

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
endif()
