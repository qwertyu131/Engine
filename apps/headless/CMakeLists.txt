# headless: offscreen render -> PNG (test gate). `headless_core` holds the renderer-independent
# pieces (CPU test pattern) so unit tests can link them; `headless` is the CLI.
#
# C++23 is inherited from golden_core (std::expected in its public headers).
add_library(headless_core STATIC
    TestPattern.h
    TestPattern.cpp)
target_include_directories(headless_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(headless_core PUBLIC golden_core)

add_executable(headless main.cpp)
target_link_libraries(headless PRIVATE headless_core golden_core engine::engine)

foreach(t headless_core headless)
    if(MSVC)
        target_compile_options(${t} PRIVATE /W4 /WX /permissive- /utf-8 /external:W0)
    else()
        target_compile_options(${t} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endforeach()
