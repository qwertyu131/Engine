# ENG_REQUIRE_RHI_BACKENDS (plan F3 Task 1): the RHI backends this build must contain. Configure fails when a
# listed backend is not built (e.g. the Vulkan SDK was not found), instead of a build that silently tests fewer
# backends. Default "vulkan;d3d11" on Windows, empty elsewhere (cmake/Dependencies.cmake).
#
# Script mode (tests/unit/scripts/test_ctest_rules.py):
#   cmake -DENG_REQUIRE_RHI_BACKENDS="vulkan;d3d11" -DENG_BUILT_RHI_BACKENDS="vulkan" -P cmake/RequireBackends.cmake

function(eng_require_rhi_backends required built)
    set(missing)
    foreach(b IN LISTS required)
        if(NOT b IN_LIST built)
            list(APPEND missing ${b})
        endif()
    endforeach()
    if(missing)
        list(JOIN missing ", " missing_text)
        list(JOIN built ", " built_text)
        message(FATAL_ERROR "ENG_REQUIRE_RHI_BACKENDS: required RHI backend(s) not built: ${missing_text} "
                            "(built: ${built_text}). Install the backend's SDK (Vulkan: VULKAN_SDK) or set "
                            "-DENG_REQUIRE_RHI_BACKENDS to the backends this build may lack.")
    endif()
endfunction()

if(CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
    eng_require_rhi_backends("${ENG_REQUIRE_RHI_BACKENDS}" "${ENG_BUILT_RHI_BACKENDS}")
endif()
