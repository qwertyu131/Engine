# apps/viewer ctest entries (plan F2 Task 6): viewer.{frames,resize,ui,click}.{vulkan,d3d11}, driven by
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
    foreach(check frames resize ui click glb)
        add_test(NAME viewer.${check}.${b}
                 COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/run_viewer.py"
                         --viewer "${eng_viewer_exe}" --backend ${b} --check ${check}
                         --work "${CMAKE_CURRENT_BINARY_DIR}/${check}_${b}")
        # Two viewer runs of <= 60 s each at most (ui); the runner itself reports a hang first.
        set_tests_properties(viewer.${check}.${b} PROPERTIES TIMEOUT 150 RUN_SERIAL TRUE SKIP_RETURN_CODE 77)
    endforeach()
endforeach()
# Plan F3 Task 8: drag-and-drop of a .glb onto the window (WM_DROPFILES posted by run_viewer.py), one backend.
if(eng_viewer_backends)
    list(GET eng_viewer_backends 0 eng_dragdrop_backend)
    add_test(NAME viewer.dragdrop
             COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/run_viewer.py"
                     --viewer "${eng_viewer_exe}" --backend ${eng_dragdrop_backend} --check dragdrop
                     --work "${CMAKE_CURRENT_BINARY_DIR}/dragdrop")
    set_tests_properties(viewer.dragdrop PROPERTIES TIMEOUT 150 RUN_SERIAL TRUE SKIP_RETURN_CODE 77)
    # The orbit camera (left drag rotates, F fits again), driven by posted Win32 mouse / key messages.
    add_test(NAME viewer.orbit
             COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/run_viewer.py"
                     --viewer "${eng_viewer_exe}" --backend ${eng_dragdrop_backend} --check orbit
                     --work "${CMAKE_CURRENT_BINARY_DIR}/orbit")
    set_tests_properties(viewer.orbit PROPERTIES TIMEOUT 400 RUN_SERIAL TRUE SKIP_RETURN_CODE 77)
endif()
