cmake_minimum_required(VERSION 3.28)

project(Engine
    VERSION 0.0.1
    DESCRIPTION "Cross-platform native glTF 2.0 renderer"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ENG_BUILD_TESTS "Build unit tests" ON)

include(cmake/Dependencies.cmake)

add_subdirectory(engine)
add_subdirectory(tools/golden)

if(ENG_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests/unit)
    add_subdirectory(tests/golden)
endif()
