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
