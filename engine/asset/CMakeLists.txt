# engine_asset: glTF 2.0 loader -> CPU ModelData (plan F3 Task 2). No GPU types. C++23 (std::expected via
# core/Result.h), like engine_rhi.
add_library(engine_asset STATIC
    ModelData.h
    GltfLoader.h
    GltfLoader.cpp
    Internal.h
    Geometry.h
    Accessors.cpp
    Primitives.cpp
    Tangents.cpp
    Images.cpp
    Extensions.h
    Extensions.cpp)
add_library(engine::asset ALIAS engine_asset)
target_compile_features(engine_asset PUBLIC cxx_std_23)
target_include_directories(engine_asset PUBLIC "${PROJECT_SOURCE_DIR}/engine")
target_link_libraries(engine_asset
    PUBLIC engine::engine
    PRIVATE fastgltf::fastgltf eng_mikktspace eng_stb eng_turbojpeg nlohmann_json::nlohmann_json)
if(MSVC)
    target_compile_options(engine_asset PRIVATE /W4 /WX /permissive- /utf-8 /external:W0)
    target_compile_definitions(engine_asset PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    target_compile_options(engine_asset PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
