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

# --- asset (engine/asset, F3 Task 2) --------------------------------------------------------------
# fastgltf v1.0.1 (tag commit 87cfbceeeda127ccc9092694279437b700046119, 2026-11-07, resolved with `git ls-remote`
# 2026-11-28), MIT. Its own simdjson / simdutf downloads are turned off: both are declared here with the release
# files and SHA256 that fastgltf v1.0.1's cmake/dependencies.cmake pins (same versions, same hashes):
#   simdjson v5.1.0 (tag commit 89f93b2ec52089f43eb3eeefce97eaa50610cc17) single-header release, Apache-2.0 / MIT;
#   simdutf  v9.3.3 (tag commit 52ac5fb6cb3a32768502e6eefa6bd2fa6f5e5b53) single-header release, Apache-2.0 / MIT,
#            base64 feature only (the defines fastgltf uses).
FetchContent_Declare(simdjson_h
    URL      https://github.com/simdjson/simdjson/releases/download/v5.1.0/simdjson.h
    URL_HASH SHA256=ca53ddca5f927b993ded4225b8fd85f18f9374e70e4f2d56376f42d661ff010a
    DOWNLOAD_NO_EXTRACT TRUE
    SOURCE_SUBDIR _no_cmake_)
FetchContent_Declare(simdjson_cpp
    URL      https://github.com/simdjson/simdjson/releases/download/v5.1.0/simdjson.cpp
    URL_HASH SHA256=0652d8dd436b210a8840267cbd0bb4c1908f6551790e7dcd543412ba54b002c9
    DOWNLOAD_NO_EXTRACT TRUE
    SOURCE_SUBDIR _no_cmake_)
FetchContent_Declare(simdutf_h
    URL      https://github.com/simdutf/simdutf/releases/download/v9.3.3/simdutf.h
    URL_HASH SHA256=90d39faf26030d18a6caec00b34a09ca4855804e7c2a9d59bf93d5d506347486
    DOWNLOAD_NO_EXTRACT TRUE
    SOURCE_SUBDIR _no_cmake_)
FetchContent_Declare(simdutf_cpp
    URL      https://github.com/simdutf/simdutf/releases/download/v9.3.3/simdutf.cpp
    URL_HASH SHA256=566033aa28edfaaa12aff8dfcfc720bc0dea0af1c0a0462bbd903c4cba1814b7
    DOWNLOAD_NO_EXTRACT TRUE
    SOURCE_SUBDIR _no_cmake_)
# MikkTSpace (mmikk/MikkTSpace master, commit 3e895b49d05ea07e4c2133156cfa94369e19e409, the repo has no tags),
# zlib-style licence (header of mikktspace.h). C source.
FetchContent_Declare(mikktspace_c
    URL      https://raw.githubusercontent.com/mmikk/MikkTSpace/3e895b49d05ea07e4c2133156cfa94369e19e409/mikktspace.c
    URL_HASH SHA256=de87e74107df766ce68108801262bd8d53899414236b59810509a8fc2a51e288
    DOWNLOAD_NO_EXTRACT TRUE
    SOURCE_SUBDIR _no_cmake_)
FetchContent_Declare(mikktspace_h
    URL      https://raw.githubusercontent.com/mmikk/MikkTSpace/3e895b49d05ea07e4c2133156cfa94369e19e409/mikktspace.h
    URL_HASH SHA256=17fc433894f24c73753d548086cc4d8c5c0379f4a6edfb98b5da243e4f0bc3d0
    DOWNLOAD_NO_EXTRACT TRUE
    SOURCE_SUBDIR _no_cmake_)
FetchContent_MakeAvailable(simdjson_h simdjson_cpp simdutf_h simdutf_cpp mikktspace_c mikktspace_h)
if(NOT TARGET simdjson::simdjson)
    add_library(eng_simdjson STATIC "${simdjson_cpp_SOURCE_DIR}/simdjson.cpp")
    target_include_directories(eng_simdjson SYSTEM PUBLIC "${simdjson_h_SOURCE_DIR}")
    target_compile_features(eng_simdjson PUBLIC cxx_std_20)
    add_library(simdjson::simdjson ALIAS eng_simdjson)
    add_library(eng_simdutf STATIC "${simdutf_cpp_SOURCE_DIR}/simdutf.cpp")
    target_include_directories(eng_simdutf SYSTEM PUBLIC "${simdutf_h_SOURCE_DIR}")
    target_compile_features(eng_simdutf PUBLIC cxx_std_20)
    target_compile_definitions(eng_simdutf PUBLIC SIMDUTF_FEATURE_BASE64=1 SIMDUTF_FEATURE_DETECT_ENCODING=0
        SIMDUTF_FEATURE_ASCII=0 SIMDUTF_FEATURE_LATIN1=0 SIMDUTF_FEATURE_UTF8=0 SIMDUTF_FEATURE_UTF16=0
        SIMDUTF_FEATURE_UTF32=0)
    add_library(simdutf::simdutf ALIAS eng_simdutf)
    if(MSVC)
        target_compile_options(eng_simdjson PRIVATE /W0 /utf-8)
        target_compile_options(eng_simdutf PRIVATE /W0 /utf-8)
    endif()
endif()
set(FASTGLTF_DOWNLOAD_SIMDJSON OFF CACHE BOOL "" FORCE)
set(FASTGLTF_DOWNLOAD_SIMDUTF OFF CACHE BOOL "" FORCE)
set(FASTGLTF_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(FASTGLTF_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(FASTGLTF_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FASTGLTF_ENABLE_DOCS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(fastgltf
    GIT_REPOSITORY https://github.com/spnda/fastgltf.git
    GIT_TAG        87cfbceeeda127ccc9092694279437b700046119 # v1.0.1
    SYSTEM)
FetchContent_MakeAvailable(fastgltf)
if(NOT TARGET eng_mikktspace)
    add_library(eng_mikktspace STATIC "${mikktspace_c_SOURCE_DIR}/mikktspace.c")
    target_include_directories(eng_mikktspace SYSTEM PUBLIC "${mikktspace_h_SOURCE_DIR}")
    if(MSVC)
        target_compile_options(eng_mikktspace PRIVATE /W0)
    endif()
endif()

# libjpeg-turbo 3.1.4 (tag commit e352b02f794f701407b39af08576035ba3360d60, resolved with `git ls-remote`
# 2026-11-28), IJG / BSD-3-Clause / zlib licences. JPEG decoder of engine/asset (plan F3 Task 2): stb_image (pinned
# above, its current master) rejects two progressive JPEGs of glTF-Sample-Assets ABeautifulGame ("bad huffman code")
# that GDI+ / WIC decode and the glTF-Validator accepts; libjpeg-turbo is also Firefox's decoder (the Sample-Viewer
# references are rendered in Firefox). TurboJPEG API, static, SIMD off (no NASM). Its CMake refuses add_subdirectory(),
# so it is an ExternalProject built per configuration; the ASan flags are passed through (it parses hostile bytes).
include(ExternalProject)
set(ENG_LJT_PREFIX "${CMAKE_BINARY_DIR}/_deps/libjpeg_turbo")
set(_ljt_c_flags "${CMAKE_C_FLAGS}")
set(_ljt_c_flags_debug "${CMAKE_C_FLAGS_DEBUG}")
set(_ljt_c_flags_release "${CMAKE_C_FLAGS_RELEASE}")
if(MSVC)
    string(APPEND _ljt_c_flags " /MP") # parallel compile; project-level -maxCpuCount comes from Directory.Build.rsp
endif()
if(ENG_ASAN)
    if(MSVC)
        string(APPEND _ljt_c_flags " /fsanitize=address /Zi")
    else()
        string(APPEND _ljt_c_flags " -fsanitize=address -fno-omit-frame-pointer")
    endif()
endif()
set(_ljt_args
    -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_SIMD=OFF -DWITH_TURBOJPEG=ON -DWITH_TOOLS=OFF -DWITH_TESTS=OFF
    -DWITH_CRT_DLL=ON -DWITH_JAVA=OFF "-DCMAKE_C_FLAGS=${_ljt_c_flags}" "-DCMAKE_C_FLAGS_DEBUG=${_ljt_c_flags_debug}"
    "-DCMAKE_C_FLAGS_RELEASE=${_ljt_c_flags_release}" -DCMAKE_POLICY_VERSION_MINIMUM=3.5)
if(CMAKE_CONFIGURATION_TYPES)
    set(_ljt_build ${CMAKE_COMMAND} --build <BINARY_DIR> --config $<CONFIG> --target turbojpeg-static)
else()
    list(APPEND _ljt_args "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}" "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}")
    set(_ljt_build ${CMAKE_COMMAND} --build <BINARY_DIR> --target turbojpeg-static)
endif()
if(MSVC)
    set(_ljt_lib "${ENG_LJT_PREFIX}/build/$<CONFIG>/turbojpeg-static.lib")
    set(_ljt_byproducts "${ENG_LJT_PREFIX}/build/Debug/turbojpeg-static.lib"
                        "${ENG_LJT_PREFIX}/build/Release/turbojpeg-static.lib"
                        "${ENG_LJT_PREFIX}/build/RelWithDebInfo/turbojpeg-static.lib"
                        "${ENG_LJT_PREFIX}/build/MinSizeRel/turbojpeg-static.lib")
else()
    set(_ljt_lib "${ENG_LJT_PREFIX}/build/libturbojpeg.a")
    set(_ljt_byproducts "${_ljt_lib}")
endif()
ExternalProject_Add(eng_libjpeg_turbo_ep
    GIT_REPOSITORY https://github.com/libjpeg-turbo/libjpeg-turbo.git
    GIT_TAG        e352b02f794f701407b39af08576035ba3360d60 # 3.1.4
    GIT_SHALLOW    FALSE
    PREFIX         "${ENG_LJT_PREFIX}"
    SOURCE_DIR     "${ENG_LJT_PREFIX}/source"
    BINARY_DIR     "${ENG_LJT_PREFIX}/build"
    STAMP_DIR      "${ENG_LJT_PREFIX}/stamp"
    TMP_DIR        "${ENG_LJT_PREFIX}/tmp"
    CMAKE_ARGS     ${_ljt_args}
    BUILD_COMMAND  ${_ljt_build}
    INSTALL_COMMAND ""
    UPDATE_COMMAND ""
    BUILD_BYPRODUCTS ${_ljt_byproducts})
file(MAKE_DIRECTORY "${ENG_LJT_PREFIX}/source/src")
if(NOT TARGET eng_turbojpeg)
    add_library(eng_turbojpeg INTERFACE)
    target_include_directories(eng_turbojpeg SYSTEM INTERFACE "${ENG_LJT_PREFIX}/source/src")
    target_link_libraries(eng_turbojpeg INTERFACE "${_ljt_lib}")
    add_dependencies(eng_turbojpeg eng_libjpeg_turbo_ep)
endif()

# --- asset decoders (engine/asset, F3 Task 3) -------------------------------------------------------------------------
# Every tag commit resolved with `git ls-remote` (2026-11-28). Built through their own CMake as static libraries,
# EXCLUDE_FROM_ALL (only what engine_asset links is built), SYSTEM includes; the ENG_ASAN flags of the top-level
# CMakeLists.txt apply to them too (they parse hostile bytes).
#   meshoptimizer v1.3 (commit 9e1f07b159d3cb777f1c67ed31fc11fd117986f4), MIT: EXT_meshopt_compression and
#     KHR_meshopt_compression buffer views (vertex codec v0 / v1, index / sequence codecs, the four filters).
#   Draco 1.5.9 (commit 554628821c3448319d7d4ae48db170b8c63e1aaf), Apache-2.0: KHR_draco_mesh_compression. Only the
#     decoder API is called (draco::Decoder); Draco's CMake has no decoder-only library, so the static `draco` library
#     is built whole and the linker keeps the decoder objects.
#   KTX-Software v4.4.2 (tag commit 4d6fc70eaf62ad0558e63e8d97eb9766118327a6), Apache-2.0 (+ the bundled Basis
#     Universal transcoder, Apache-2.0): libktx's read-only library `ktx_read` (KTX2 parse, DFD, Basis ETC1S / UASTC
#     transcode) for KHR_texture_basisu. Tools, tests, docs, GL / Vulkan upload are off; KTX1 stays on (ktx_read
#     does not link without it).
#   libwebp v1.6.0 (tag commit 4fa21912338357f89e4fd51cf2368325b59e9bd9), BSD-3-Clause: `webpdecoder` (decode only)
#     for EXT_texture_webp. Tools, mux, extras off.
set(MESHOPT_INSTALL OFF CACHE BOOL "" FORCE)
set(MESHOPT_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(meshoptimizer
    GIT_REPOSITORY https://github.com/zeux/meshoptimizer.git
    GIT_TAG        9e1f07b159d3cb777f1c67ed31fc11fd117986f4 # v1.3
    EXCLUDE_FROM_ALL
    SYSTEM)
set(DRACO_BUILD_EXECUTABLES OFF CACHE BOOL "" FORCE)
set(DRACO_TESTS OFF CACHE BOOL "" FORCE)
set(DRACO_INSTALL OFF CACHE BOOL "" FORCE)
set(DRACO_JS_GLUE OFF CACHE BOOL "" FORCE)
set(DRACO_TRANSCODER_SUPPORTED OFF CACHE BOOL "" FORCE)
set(DRACO_UNITY_PLUGIN OFF CACHE BOOL "" FORCE)
set(DRACO_MAYA_PLUGIN OFF CACHE BOOL "" FORCE)
FetchContent_Declare(draco
    GIT_REPOSITORY https://github.com/google/draco.git
    GIT_TAG        554628821c3448319d7d4ae48db170b8c63e1aaf # 1.5.9
    EXCLUDE_FROM_ALL
    SYSTEM)
foreach(_eng_webp_opt ANIM_UTILS CWEBP DWEBP GIF2WEBP IMG2WEBP VWEBP WEBPINFO LIBWEBPMUX WEBPMUX EXTRAS)
    set(WEBP_BUILD_${_eng_webp_opt} OFF CACHE BOOL "" FORCE)
endforeach()
FetchContent_Declare(libwebp
    GIT_REPOSITORY https://github.com/webmproject/libwebp.git
    GIT_TAG        4fa21912338357f89e4fd51cf2368325b59e9bd9 # v1.6.0
    EXCLUDE_FROM_ALL
    SYSTEM)
set(KTX_FEATURE_TESTS OFF CACHE BOOL "" FORCE)
set(KTX_FEATURE_TOOLS OFF CACHE BOOL "" FORCE)
set(KTX_FEATURE_DOC OFF CACHE BOOL "" FORCE)
set(KTX_FEATURE_JNI OFF CACHE BOOL "" FORCE)
set(KTX_FEATURE_PY OFF CACHE BOOL "" FORCE)
set(KTX_FEATURE_GL_UPLOAD OFF CACHE BOOL "" FORCE)
set(KTX_FEATURE_VK_UPLOAD OFF CACHE BOOL "" FORCE)
set(KTX_FEATURE_KTX1 ON CACHE BOOL "" FORCE)
set(KTX_FEATURE_KTX2 ON CACHE BOOL "" FORCE)
set(KTX_FEATURE_LOADTEST_APPS "" CACHE STRING "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Static libraries (dependencies)" FORCE)
FetchContent_Declare(ktx_software
    GIT_REPOSITORY https://github.com/KhronosGroup/KTX-Software.git
    GIT_TAG        4d6fc70eaf62ad0558e63e8d97eb9766118327a6 # v4.4.2
    GIT_SHALLOW    FALSE
    EXCLUDE_FROM_ALL
    SYSTEM)
FetchContent_MakeAvailable(meshoptimizer draco libwebp ktx_software)
if(NOT TARGET eng_draco)
    add_library(eng_draco INTERFACE)
    target_link_libraries(eng_draco INTERFACE draco::draco)
    # draco/draco_features.h is generated into the build tree.
    target_include_directories(eng_draco SYSTEM INTERFACE "${draco_SOURCE_DIR}/src" "${draco_BINARY_DIR}")
    add_library(eng_webp INTERFACE)
    target_link_libraries(eng_webp INTERFACE webpdecoder)
    target_include_directories(eng_webp SYSTEM INTERFACE "${libwebp_SOURCE_DIR}/src")
endif()

if(ENG_BUILD_TESTS)
    # bcdec (iOrange/bcdec master, commit 80859ed3b7afb1c527a2a99d70c61457bea72d0c, v0.985; the repo has no tags),
    # MIT / Unlicense: CPU BC1..BC7 block decoder of the tests only (plan F3 Task 3: KTX2 -> BC7 -> CPU decode).
    FetchContent_Declare(bcdec
        URL      https://raw.githubusercontent.com/iOrange/bcdec/80859ed3b7afb1c527a2a99d70c61457bea72d0c/bcdec.h
        URL_HASH SHA256=134520764d96f70a27db814173616c89ad85db95fbd3328417c7a8c77e7ca189
        DOWNLOAD_NO_EXTRACT TRUE
        SOURCE_SUBDIR _no_cmake_)
    FetchContent_MakeAvailable(bcdec)
    if(NOT TARGET eng_bcdec)
        add_library(eng_bcdec INTERFACE)
        target_include_directories(eng_bcdec SYSTEM INTERFACE "${bcdec_SOURCE_DIR}")
    endif()
endif()
