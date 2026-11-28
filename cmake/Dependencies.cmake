# Third-party dependencies. Every entry is pinned to an exact commit SHA (tag noted alongside).
include(FetchContent)

if(ENG_BUILD_TESTS)
    # doctest v2.5.3 (commit 2d0a9359a60c51affe2a9bebb1be1dca47868151), MIT.
    # Only the single header is used; doctest's own CMake project is not configured
    # (SOURCE_SUBDIR points to a directory without a CMakeLists.txt).
    FetchContent_Declare(doctest
        GIT_REPOSITORY https://github.com/doctest/doctest.git
        GIT_TAG        2d0a9359a60c51affe2a9bebb1be1dca47868151 # v2.5.3
        SOURCE_SUBDIR  _no_cmake_)
    FetchContent_MakeAvailable(doctest)

    if(NOT TARGET doctest::doctest)
        add_library(eng_doctest INTERFACE)
        add_library(doctest::doctest ALIAS eng_doctest)
        target_include_directories(eng_doctest SYSTEM INTERFACE "${doctest_SOURCE_DIR}")
    endif()
endif()

# --- golden tool (tools/golden) -------------------------------------------------------------
# Single-file / release-archive dependencies: pinned by commit SHA in the URL plus SHA256.

# NVIDIA FLIP single header (src/cpp/FLIP.h) at commit d3ab02a4425eb167d8a4021b382a2bafc6e6c838
# (master, 2026-10-13; the repo has no tags), BSD-3-Clause.
FetchContent_Declare(flip
    URL      https://raw.githubusercontent.com/NVlabs/flip/d3ab02a4425eb167d8a4021b382a2bafc6e6c838/src/cpp/FLIP.h
    URL_HASH SHA256=c6dfe508adb7802a9f602114ece3d822dccb739ba389b97320c91cf2b1ddbc07
    DOWNLOAD_NO_EXTRACT TRUE
    SOURCE_SUBDIR _no_cmake_)

# stb_image v2.34 / stb_image_write at commit 6b5799bae8a1fe3792691ef4b72ae26434c3e799,
# public domain / MIT.
FetchContent_Declare(stb_image
    URL      https://raw.githubusercontent.com/nothings/stb/6b5799bae8a1fe3792691ef4b72ae26434c3e799/stb_image.h
    URL_HASH SHA256=dfc2aadda6295f67f677587773f25e5bf8c8ad427ed0db7c518525c91e72e020
    DOWNLOAD_NO_EXTRACT TRUE
    SOURCE_SUBDIR _no_cmake_)
FetchContent_Declare(stb_image_write
    URL      https://raw.githubusercontent.com/nothings/stb/6b5799bae8a1fe3792691ef4b72ae26434c3e799/stb_image_write.h
    URL_HASH SHA256=5d89edc8bffd848f3e138b2a1dbec67aadbed591894bde16ade7c83dc233af9b
    DOWNLOAD_NO_EXTRACT TRUE
    SOURCE_SUBDIR _no_cmake_)

# nlohmann/json v3.12.0 (tag commit 55f93686c01528224f448c19128836e7df245f72), MIT.
# Official release archive json.tar.xz; header-only, its own CMake project is not configured.
FetchContent_Declare(nlohmann_json
    URL      https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
    SOURCE_SUBDIR _no_cmake_)

FetchContent_MakeAvailable(flip stb_image stb_image_write nlohmann_json)

if(NOT TARGET eng_flip)
    add_library(eng_flip INTERFACE)
    target_include_directories(eng_flip SYSTEM INTERFACE "${flip_SOURCE_DIR}")
    add_library(eng_stb INTERFACE)
    target_include_directories(eng_stb SYSTEM INTERFACE "${stb_image_SOURCE_DIR}" "${stb_image_write_SOURCE_DIR}")
endif()
if(NOT TARGET nlohmann_json::nlohmann_json)
    add_library(eng_nlohmann_json INTERFACE)
    add_library(nlohmann_json::nlohmann_json ALIAS eng_nlohmann_json)
    target_include_directories(eng_nlohmann_json SYSTEM INTERFACE "${nlohmann_json_SOURCE_DIR}/include")
endif()

# --- shaderc (tools/shaderc, F1) ------------------------------------------------------------
# SPIRV-Cross as a library. One pin: the commit is read from third_party/tools/tools.lock.json
# ("spirv-cross"."commit"; tag vulkan-sdk-1.4.341.0 = commit a0fba56c34a6700f1724bf9b751da5b488a3775c,
# matching the Vulkan SDK 1.4.341.1 tools), Apache-2.0.
if(ENG_BUILD_SHADERC)
    file(READ "${PROJECT_SOURCE_DIR}/third_party/tools/tools.lock.json" _eng_tools_lock)
    string(JSON ENG_SPIRV_CROSS_COMMIT GET "${_eng_tools_lock}" "spirv-cross" "commit")
    set(SPIRV_CROSS_CLI OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_SHARED OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_STATIC ON CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_C_API OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_UTIL OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_CPP OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_SKIP_INSTALL ON CACHE BOOL "" FORCE)
    FetchContent_Declare(spirv_cross
        GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Cross.git
        GIT_TAG        ${ENG_SPIRV_CROSS_COMMIT}
        GIT_SHALLOW    FALSE
        SYSTEM)
    FetchContent_MakeAvailable(spirv_cross)
endif()

# --- RHI (engine/rhi, F2) -------------------------------------------------------------------
# Vulkan backend: Vulkan headers from the Vulkan SDK (find_package(Vulkan), headers only; the loader is
# loaded at run time by volk). Neither project's own CMake is configured (SOURCE_SUBDIR): volk is used
# header-only (VOLK_IMPLEMENTATION in engine/rhi/vulkan/VkLibs.cpp), VMA is header-only.
# volk at tag vulkan-sdk-1.4.341.0 (commit d979819fd03de3a7606d6e23d6ff4968942599da), MIT.
# VulkanMemoryAllocator v3.5.0 (commit 0f1a757da579831283413295cc7f3b47eb521c77), MIT.
# Both SHAs resolved with `git ls-remote` (2026-11-28).
if(WIN32)
    option(ENG_RHI_D3D11 "Build the D3D11 RHI backend" ON)
else()
    set(ENG_RHI_D3D11 OFF CACHE BOOL "Build the D3D11 RHI backend (Windows only)" FORCE)
endif()
find_package(Vulkan QUIET)
if(TARGET Vulkan::Headers)
    option(ENG_RHI_VULKAN "Build the Vulkan RHI backend" ON)
else()
    message(STATUS "Vulkan headers not found (VULKAN_SDK): the Vulkan RHI backend is not built")
    set(ENG_RHI_VULKAN OFF CACHE BOOL "Build the Vulkan RHI backend" FORCE)
endif()
if(ENG_RHI_VULKAN)
    FetchContent_Declare(volk
        GIT_REPOSITORY https://github.com/zeux/volk.git
        GIT_TAG        d979819fd03de3a7606d6e23d6ff4968942599da # vulkan-sdk-1.4.341.0
        SOURCE_SUBDIR  _no_cmake_)
    FetchContent_Declare(vma
        GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
        GIT_TAG        0f1a757da579831283413295cc7f3b47eb521c77 # v3.5.0
        SOURCE_SUBDIR  _no_cmake_)
    FetchContent_MakeAvailable(volk vma)
    if(NOT TARGET eng_volk)
        add_library(eng_volk INTERFACE)
        target_include_directories(eng_volk SYSTEM INTERFACE "${volk_SOURCE_DIR}")
        target_compile_definitions(eng_volk INTERFACE VK_NO_PROTOTYPES)
        target_link_libraries(eng_volk INTERFACE Vulkan::Headers)
        add_library(eng_vma INTERFACE)
        target_include_directories(eng_vma SYSTEM INTERFACE "${vma_SOURCE_DIR}/include")
    endif()
endif()

# Required backends (plan F3 Task 1, cmake/RequireBackends.cmake): configure fails when one is not built.
if(WIN32)
    set(ENG_REQUIRE_RHI_BACKENDS "vulkan;d3d11" CACHE STRING "RHI backends this build must contain (configure fails otherwise)")
else()
    set(ENG_REQUIRE_RHI_BACKENDS "" CACHE STRING "RHI backends this build must contain (configure fails otherwise)")
endif()
set(ENG_BUILT_RHI_BACKENDS)
if(ENG_RHI_VULKAN)
    list(APPEND ENG_BUILT_RHI_BACKENDS vulkan)
endif()
if(ENG_RHI_D3D11)
    list(APPEND ENG_BUILT_RHI_BACKENDS d3d11)
endif()
include("${CMAKE_CURRENT_LIST_DIR}/RequireBackends.cmake")
eng_require_rhi_backends("${ENG_REQUIRE_RHI_BACKENDS}" "${ENG_BUILT_RHI_BACKENDS}")

# --- UI (engine/ui, F2 Task 6) --------------------------------------------------------------
# Dear ImGui v1.92.9b (tag commit f1cc2ae15e53a861a874c3034aae6798fde194ab, resolved with `git ls-remote`
# 2026-11-28), MIT. Core files only (imgui*.cpp); no backend from imgui's backends/ directory: ImGui is
# drawn through engine/rhi (engine/ui/ImGuiRenderer.cpp) and fed by engine/platform input events.
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        f1cc2ae15e53a861a874c3034aae6798fde194ab # v1.92.9b
    SOURCE_SUBDIR  _no_cmake_)
FetchContent_MakeAvailable(imgui)
if(NOT TARGET eng_imgui)
    add_library(eng_imgui STATIC
        "${imgui_SOURCE_DIR}/imgui.cpp"
        "${imgui_SOURCE_DIR}/imgui_draw.cpp"
        "${imgui_SOURCE_DIR}/imgui_tables.cpp"
        "${imgui_SOURCE_DIR}/imgui_widgets.cpp")
    target_include_directories(eng_imgui SYSTEM PUBLIC "${imgui_SOURCE_DIR}")
    # No imgui_demo.cpp: the demo window is not part of the viewer.
    target_compile_definitions(eng_imgui PUBLIC IMGUI_DISABLE_DEMO_WINDOWS IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
    if(MSVC)
        target_compile_options(eng_imgui PRIVATE /W0 /utf-8)
    endif()
endif()
