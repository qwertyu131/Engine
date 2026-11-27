# engine_rhi: backend-independent RHI + the backends compiled into this build (plan F2 Task 2).
#
# C++23 for this target: the pinned interface (plan F2 Task 2) uses std::expected (<expected> is C++23;
# MSVC does not provide it under /std:c++20), as tools/golden does. The `engine` core library stays C++20.
add_library(engine_rhi STATIC
    Rhi.h
    Types.h
    Types.cpp
    DeviceCaps.h
    HandlePool.h
    Registry.cpp
    ShaderLibrary.h
    ShaderLibrary.cpp)
add_library(engine::rhi ALIAS engine_rhi)
target_compile_features(engine_rhi PUBLIC cxx_std_23)
target_link_libraries(engine_rhi PUBLIC engine::engine PRIVATE nlohmann_json::nlohmann_json)

if(ENG_RHI_VULKAN)
    target_sources(engine_rhi PRIVATE
        vulkan/VkCommon.h
        vulkan/VkLibs.cpp
        vulkan/VkDevice.cpp
        vulkan/VkCommandList.cpp
        vulkan/VkResources.cpp
        vulkan/VkValidation.cpp)
    target_link_libraries(engine_rhi PRIVATE eng_volk eng_vma)
    target_compile_definitions(engine_rhi PUBLIC ENG_RHI_HAS_VULKAN=1)
    if(WIN32)
        target_compile_definitions(engine_rhi PRIVATE VK_USE_PLATFORM_WIN32_KHR)
    endif()
endif()
if(ENG_RHI_D3D11)
    target_sources(engine_rhi PRIVATE
        d3d11/D3D11Common.h
        d3d11/D3D11Device.cpp
        d3d11/D3D11CommandList.cpp
        d3d11/D3D11Resources.cpp
        d3d11/D3D11Validation.cpp)
    target_link_libraries(engine_rhi PRIVATE d3d11 dxgi dxguid)
    target_compile_definitions(engine_rhi PUBLIC ENG_RHI_HAS_D3D11=1)
endif()

if(MSVC)
    target_compile_options(engine_rhi PRIVATE /W4 /WX /permissive- /utf-8 /external:W0)
    target_compile_definitions(engine_rhi PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    target_compile_options(engine_rhi PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
