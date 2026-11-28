# headless: offscreen render -> PNG (test gate). `headless_core` holds the CPU test pattern (backend `cpu`) so unit
# tests can link it; every GPU scene goes through the library facade engine::facade (include/engine, spec K-D, plan F3
# Task 4: the F2 built-in scenes now live in engine/renderer/builtin).
#
# C++23 is inherited from golden_core / engine_facade (std::expected in their public headers).
add_library(headless_core STATIC
    TestPattern.h
    TestPattern.cpp)
target_include_directories(headless_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(headless_core PUBLIC golden_core)

add_executable(headless main.cpp)
# K-D: the facade only (its public include path is include/; tests/unit/scripts/test_facade_includes.py checks it).
target_link_libraries(headless PRIVATE engine::facade headless_core golden_core)
# Default --shader-dir: the build tree's shader library (UTF-8 path; plan F2 Task 2).
if(TARGET shaders)
    target_compile_definitions(headless PRIVATE ENG_DEFAULT_SHADER_DIR="${ENG_SHADER_LIB_DIR}")
    add_dependencies(headless shaders)
endif()
# Default --asset-dir: the source tree's tests/assets (UTF-8 path; plan F2 Task 3).
target_compile_definitions(headless PRIVATE ENG_DEFAULT_ASSET_DIR="${PROJECT_SOURCE_DIR}/tests/assets")

foreach(t headless_core headless)
    if(MSVC)
        target_compile_options(${t} PRIVATE /W4 /WX /permissive- /utf-8 /external:W0)
    else()
        target_compile_options(${t} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endforeach()
