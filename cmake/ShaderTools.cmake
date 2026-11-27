# Offline shader toolchain (plan F1 Task 1). Exports cache variables with absolute tool paths:
#   ENG_DXC        third_party/tools/dxc/bin/x64/dxc.exe   (pinned release; never the Vulkan SDK copy)
#   ENG_TINT       third_party/tools/tint/tint.exe         (built from the pinned Dawn commit)
#   ENG_FXC        Windows SDK 10.0.26100 bin/x64/fxc.exe
#   ENG_SPIRV_VAL  $ENV{VULKAN_SDK}/Bin/spirv-val.exe
#   ENG_GLSLANG    $ENV{VULKAN_SDK}/Bin/glslangValidator.exe
# Pins: third_party/tools/tools.lock.json; fetch/build: python scripts/fetch_shader_tools.py.
# A missing tool is a configure WARNING (not an error); shaderc tests that need it are reported as
# skipped with the reason at test time, never silently passed.
set(ENG_SHADER_TOOLS_DIR "${PROJECT_SOURCE_DIR}/third_party/tools")
set(ENG_WINDOWS_SDK_VERSION "10.0.26100.0")

function(eng_find_shader_tool var name)
    cmake_parse_arguments(PARSE_ARGV 2 T "" "" "PATHS")
    # NO_DEFAULT_PATH: only the given locations; a same-named tool elsewhere on PATH is never used.
    # Searched on every configure (a tool fetched after the first configure is picked up).
    find_program(_eng_found NAMES ${name} PATHS ${T_PATHS} NO_DEFAULT_PATH NO_CACHE)
    if(NOT _eng_found)
        message(WARNING "tool not found: ${name} (run scripts/fetch_shader_tools.py); "
                        "shaderc tests needing it will be skipped")
        set(${var} "" CACHE FILEPATH "${name} for shaderc" FORCE)
    else()
        set(${var} "${_eng_found}" CACHE FILEPATH "${name} for shaderc" FORCE)
        message(STATUS "shader tool ${name}: ${_eng_found}")
    endif()
endfunction()

set(_eng_pf86 "ProgramFiles(x86)")
set(_eng_kits "$ENV{${_eng_pf86}}/Windows Kits/10")
set(_eng_vk "$ENV{VULKAN_SDK}")

eng_find_shader_tool(ENG_DXC dxc PATHS "${ENG_SHADER_TOOLS_DIR}/dxc/bin/x64")
eng_find_shader_tool(ENG_TINT tint PATHS "${ENG_SHADER_TOOLS_DIR}/tint")
eng_find_shader_tool(ENG_FXC fxc PATHS "${_eng_kits}/bin/${ENG_WINDOWS_SDK_VERSION}/x64")
if(_eng_vk)
    eng_find_shader_tool(ENG_SPIRV_VAL spirv-val PATHS "${_eng_vk}/Bin" "${_eng_vk}/bin")
    eng_find_shader_tool(ENG_GLSLANG glslangValidator PATHS "${_eng_vk}/Bin" "${_eng_vk}/bin")
else()
    message(WARNING "VULKAN_SDK is not set: tool not found: spirv-val, glslangValidator")
    set(ENG_SPIRV_VAL "" CACHE FILEPATH "spirv-val for shaderc" FORCE)
    set(ENG_GLSLANG "" CACHE FILEPATH "glslangValidator for shaderc" FORCE)
endif()
