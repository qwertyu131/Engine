# viewer: windowed renderer with an RHI-drawn ImGui (plan F2 Task 6). Windows only for now (Win32 window).
# K-D (plan F3 Task 4): the facade only (engine::facade, include/engine; Dear ImGui comes with it for the panels).
add_executable(viewer main.cpp Panels.h Panels.cpp)
target_link_libraries(viewer PRIVATE engine::facade golden_core)
if(TARGET shaders)
    target_compile_definitions(viewer PRIVATE ENG_DEFAULT_SHADER_DIR="${ENG_SHADER_LIB_DIR}")
    add_dependencies(viewer shaders)
endif()
target_compile_definitions(viewer PRIVATE ENG_DEFAULT_ASSET_DIR="${PROJECT_SOURCE_DIR}/tests/assets")
if(MSVC)
    target_compile_options(viewer PRIVATE /W4 /WX /permissive- /utf-8 /external:W0)
    target_link_options(viewer PRIVATE /SUBSYSTEM:CONSOLE)
endif()
