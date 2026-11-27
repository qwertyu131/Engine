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
