# engine_renderer: GPU side of the glTF renderer (plan F3 Task 3: texture upload / KTX2 transcode target; Task 4: GPU
# model, draw lists, passes, and the F2 built-in scenes, moved here from apps/headless so the facade can serve them).
# Uses engine/asset (CPU ModelData), engine/scene and engine/rhi. C++23 (std::expected via core/Result.h).
add_library(engine_renderer STATIC
    TextureUpload.h
    TextureUpload.cpp
    GpuModel.h
    GpuModel.cpp
    DrawList.h
    DrawList.cpp
    Passes.h
    Passes.cpp
    Ibl.h
    Ibl.cpp
    IblCpu.cpp
    Skybox.h
    Skybox.cpp
    Tonemap.cpp
    RenderSettings.h
    RenderSettings.cpp
    Lights.h
    Lights.cpp
    Shadows.h
    Shadows.cpp
    builtin/Scenes.h
    builtin/Scenes.cpp
    builtin/SceneUtil.h
    builtin/CubeScene.cpp
    builtin/Ibl.h
    builtin/Ibl.cpp
    builtin/PbrSpheresScene.cpp)
add_library(engine::renderer ALIAS engine_renderer)
# Config/DefaultScalability.ini (sg.* table, plan F3 Task 7) embedded as a string: no run-time file lookup.
file(READ "${PROJECT_SOURCE_DIR}/Config/DefaultScalability.ini" ENG_SCALABILITY_INI)
configure_file(ScalabilityIni.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/ScalabilityIni.h" @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/Config/DefaultScalability.ini")
target_include_directories(engine_renderer PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_features(engine_renderer PUBLIC cxx_std_23)
target_include_directories(engine_renderer PUBLIC "${PROJECT_SOURCE_DIR}/engine")
# golden_core: PNG loading of builtin:textured-cube's atlas (as in F2).
target_link_libraries(engine_renderer PUBLIC engine::engine engine::asset engine::scene engine::rhi PRIVATE golden_core eng_stb)

# engine_facade: the public library facade (spec K-D): include/engine/*.h, implemented in Renderer.cpp. Hosts link only
# this target; every engine library is a PRIVATE (link-only) dependency, so a host's include path is include/ (plus
# Dear ImGui for the debug UI) and nothing of engine/.
add_library(engine_facade STATIC
    "${PROJECT_SOURCE_DIR}/include/engine/AssetHandle.h"
    "${PROJECT_SOURCE_DIR}/include/engine/Camera.h"
    "${PROJECT_SOURCE_DIR}/include/engine/CVar.h"
    "${PROJECT_SOURCE_DIR}/include/engine/Renderer.h"
    "${PROJECT_SOURCE_DIR}/include/engine/Result.h"
    "${PROJECT_SOURCE_DIR}/include/engine/Scene.h"
    "${PROJECT_SOURCE_DIR}/include/engine/Settings.h"
    "${PROJECT_SOURCE_DIR}/include/engine/Window.h"
    "${PROJECT_SOURCE_DIR}/include/engine/rhi/DeviceCaps.h"
    "${PROJECT_SOURCE_DIR}/include/engine/rhi/Types.h"
    Renderer.cpp)
add_library(engine::facade ALIAS engine_facade)
target_compile_features(engine_facade PUBLIC cxx_std_23)
target_include_directories(engine_facade PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(engine_facade PUBLIC eng_imgui PRIVATE engine_renderer engine_ui)
if(TARGET engine_platform)
    target_link_libraries(engine_facade PRIVATE engine_platform)
endif()

foreach(t engine_renderer engine_facade)
    if(MSVC)
        target_compile_options(${t} PRIVATE /W4 /WX /permissive- /utf-8 /external:W0)
        target_compile_definitions(${t} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    else()
        target_compile_options(${t} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endforeach()
