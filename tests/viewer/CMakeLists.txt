# apps/viewer ctest entries (plan F2 Task 6): viewer.{frames,resize,ui}.{vulkan,d3d11}, driven by
# tests/viewer/run_viewer.py (pixel checks on the screenshots; a run that does not exit within 60 s is
# reported as a hang, never killed). The viewer opens a real window on the desktop.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
if(TARGET viewer)
    set(eng_viewer_exe "$<TARGET_FILE:viewer>")
else()
    # Not built: the entries still exist and fail (the runner cannot start the viewer).
    set(eng_viewer_exe "${CMAKE_BINARY_DIR}/apps/viewer/viewer-not-built.exe")
endif()
set(eng_viewer_backends)
if(ENG_RHI_VULKAN)
    list(APPEND eng_viewer_backends vulkan)
endif()
if(ENG_RHI_D3D11)
    list(APPEND eng_viewer_backends d3d11)
endif()
foreach(b IN LISTS eng_viewer_backends)
    foreach(check frames resize ui)
        add_test(NAME viewer.${check}.${b}
                 COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/run_viewer.py"
                         --viewer "${eng_viewer_exe}" --backend ${b} --check ${check}
                         --work "${CMAKE_CURRENT_BINARY_DIR}/${check}_${b}")
        # Two viewer runs of <= 60 s each at most (ui); the runner itself reports a hang first.
        set_tests_properties(viewer.${check}.${b} PROPERTIES TIMEOUT 150 RUN_SERIAL TRUE)
    endforeach()
endforeach()
