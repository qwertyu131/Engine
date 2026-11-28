# engine_scene: node hierarchy, world transforms, skins / morph weights at rest pose, the fit camera (plan F3 Task 4).
# CPU only. C++23 (std::expected via core/Result.h).
add_library(engine_scene STATIC
    Math.h
    Scene.h
    Scene.cpp
    FitCamera.cpp)
add_library(engine::scene ALIAS engine_scene)
target_compile_features(engine_scene PUBLIC cxx_std_23)
target_include_directories(engine_scene PUBLIC "${PROJECT_SOURCE_DIR}/engine")
target_link_libraries(engine_scene PUBLIC engine::engine engine::asset)
if(MSVC)
    target_compile_options(engine_scene PRIVATE /W4 /WX /permissive- /utf-8 /external:W0)
else()
    target_compile_options(engine_scene PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
