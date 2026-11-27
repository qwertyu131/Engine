# Golden image diff tool: library `golden_core` (load/save/compare/batch) + CLI `golden`.
#
# C++23 for these targets only: the plan's interface uses std::expected (<expected> is C++23;
# MSVC does not provide it under /std:c++20). The `engine` library stays C++20.
add_library(golden_core STATIC
    Image.h
    Image.cpp
    Compare.h
    Compare.cpp
    Batch.h
    Batch.cpp
    Utf8.h)
target_include_directories(golden_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(golden_core PUBLIC cxx_std_23)
target_link_libraries(golden_core PRIVATE eng_flip eng_stb nlohmann_json::nlohmann_json)

add_executable(golden main.cpp)
target_link_libraries(golden PRIVATE golden_core nlohmann_json::nlohmann_json)

foreach(t golden_core golden)
    if(MSVC)
        target_compile_options(${t} PRIVATE /W4 /WX /permissive- /utf-8 /external:W0)
    else()
        target_compile_options(${t} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endforeach()
