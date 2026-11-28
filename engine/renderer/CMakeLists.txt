# engine_renderer: GPU side of the glTF renderer (plan F3 Task 3: texture upload / KTX2 transcode target; Task 4: scene
# upload, draw lists, passes, the public facade). Uses engine/asset (CPU ModelData) and engine/rhi. C++23 (std::expected
# via core/Result.h), like engine_rhi.
add_library(engine_renderer STATIC
    TextureUpload.h
    TextureUpload.cpp)
add_library(engine::renderer ALIAS engine_renderer)
target_compile_features(engine_renderer PUBLIC cxx_std_23)
target_include_directories(engine_renderer PUBLIC "${PROJECT_SOURCE_DIR}/engine")
target_link_libraries(engine_renderer PUBLIC engine::engine engine::asset engine::rhi)
if(MSVC)
    target_compile_options(engine_renderer PRIVATE /W4 /WX /permissive- /utf-8 /external:W0)
    target_compile_definitions(engine_renderer PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    target_compile_options(engine_renderer PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
