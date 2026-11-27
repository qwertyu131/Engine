# d3d11_linkcheck: D3D11 debug-layer check of one VS + PS DXBC pair (plan F1 Task 5 spike tool,
# committed in plan F2 Task 1). Windows-only, optional (ENG_BUILD_D3D11_LINKCHECK), not part of ctest:
#   d3d11_linkcheck <vs.dxbc> <ps.dxbc> [--hardware]   -> exit 0 clean, 1 debug-layer messages, 2 error
add_executable(d3d11_linkcheck main.cpp)
target_link_libraries(d3d11_linkcheck PRIVATE d3d11 d3dcompiler dxguid)
target_compile_features(d3d11_linkcheck PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(d3d11_linkcheck PRIVATE /W4 /WX /permissive- /utf-8)
endif()
