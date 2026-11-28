add_library(engine STATIC
    core/Assert.h
    core/CVar.h
    core/CVar.cpp
    core/Log.h
    core/Log.cpp)
add_library(engine::engine ALIAS engine)

# Public facade headers (include/engine, spec K-D, plan F3 Task 4) + the engine-internal tree.
target_include_directories(engine PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/include")
target_compile_features(engine PUBLIC cxx_std_20)

# Asserts: DEBUG_ONLY (default) = active in Debug config only; ON / OFF force it.
set(ENG_ENABLE_ASSERTS "DEBUG_ONLY" CACHE STRING "ON, OFF or DEBUG_ONLY")
set_property(CACHE ENG_ENABLE_ASSERTS PROPERTY STRINGS ON OFF DEBUG_ONLY)
if(ENG_ENABLE_ASSERTS STREQUAL "DEBUG_ONLY")
    target_compile_definitions(engine PUBLIC "ENG_ENABLE_ASSERTS=$<IF:$<CONFIG:Debug>,1,0>")
elseif(ENG_ENABLE_ASSERTS)
    target_compile_definitions(engine PUBLIC ENG_ENABLE_ASSERTS=1)
else()
    target_compile_definitions(engine PUBLIC ENG_ENABLE_ASSERTS=0)
endif()

if(MSVC)
    target_compile_options(engine PRIVATE /W4 /WX /permissive-)
    # Source files are UTF-8; the Windows system codepage may not be (Turkish locale).
    target_compile_options(engine PUBLIC /utf-8)
else()
    target_compile_options(engine PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

# RHI (plan F2 Task 2): separate target, C++23 (std::expected in the pinned interface).
add_subdirectory(rhi)

# glTF loader (plan F3 Task 2): separate target, C++23, no GPU types.
add_subdirectory(asset)

# Scene + renderer (plan F3 Tasks 3-4): rest-pose scene graph, GPU upload, draw lists, passes, facade.
add_subdirectory(scene)
add_subdirectory(renderer)

# Window + UI (plan F2 Task 6).
add_subdirectory(platform)
add_subdirectory(ui)
