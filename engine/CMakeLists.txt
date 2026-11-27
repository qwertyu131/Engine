add_library(engine STATIC
    core/Assert.h
    core/CVar.h
    core/CVar.cpp
    core/Log.h
    core/Log.cpp)
add_library(engine::engine ALIAS engine)

target_include_directories(engine PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
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
