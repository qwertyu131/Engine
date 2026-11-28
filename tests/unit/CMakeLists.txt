add_executable(engine_unit_tests
    test_main.cpp
    core/test_log.cpp
    core/test_cvar.cpp
    golden/test_compare.cpp
    headless/test_pattern.cpp)

target_link_libraries(engine_unit_tests PRIVATE engine::engine golden_core headless_core nlohmann_json::nlohmann_json doctest::doctest)
target_compile_definitions(engine_unit_tests PRIVATE
    ENG_GOLDEN_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/golden/fixtures")

# One CTest entry per doctest suite, so `ctest -R <suite>` selects real test cases.
set(ENG_UNIT_SUITES log assert cvar golden headless)
foreach(suite IN LISTS ENG_UNIT_SUITES)
    add_test(NAME unit.${suite}
             COMMAND engine_unit_tests --test-suite=${suite} --no-version=true)
    # doctest exits 0 when a filter matches nothing; treat an empty suite as a failure.
    set_tests_properties(unit.${suite} PROPERTIES
        FAIL_REGULAR_EXPRESSION "test cases: +0 \\|")
endforeach()

# Catch-all: every case in the binary (guards against cases outside a listed suite).
add_test(NAME unit.all COMMAND engine_unit_tests --no-version=true)

# --- asset (F3 Task 2) -------------------------------------------------------------------------------
# glTF loader tests: Sample-Assets (tests/assets/glTF-Sample-Assets, scripts/fetch_assets.py), the F3 fixtures
# (tests/assets/f03, scripts/make_f03_assets.py) and the pinned glTF-Validator for the conformance suite. A missing
# asset checkout or validator is a failure with a fetch hint, never a skip.
file(READ "${PROJECT_SOURCE_DIR}/tests/assets/assets.lock.json" _eng_assets_lock)
string(JSON ENG_GLTF_VALIDATOR_VERSION GET "${_eng_assets_lock}" "gltf-validator" "version")
if(WIN32)
    set(ENG_GLTF_VALIDATOR "${PROJECT_SOURCE_DIR}/tests/assets/tools/gltf_validator-${ENG_GLTF_VALIDATOR_VERSION}-win64/gltf_validator.exe")
else()
    set(ENG_GLTF_VALIDATOR "${PROJECT_SOURCE_DIR}/tests/assets/tools/gltf_validator-${ENG_GLTF_VALIDATOR_VERSION}-linux64/gltf_validator")
endif()
add_executable(asset_unit_tests
    asset/test_main.cpp
    asset/AssetTestEnv.h
    asset/test_loader.cpp
    asset/test_bad_files.cpp
    asset/test_conformance.cpp
    asset/test_decoders.cpp)
target_link_libraries(asset_unit_tests PRIVATE engine::asset nlohmann_json::nlohmann_json doctest::doctest eng_draco meshoptimizer eng_bcdec)
target_compile_definitions(asset_unit_tests PRIVATE
    ENG_TEST_F03_ASSETS="${PROJECT_SOURCE_DIR}/tests/assets/f03"
    ENG_TEST_SAMPLE_MODELS="${PROJECT_SOURCE_DIR}/tests/assets/glTF-Sample-Assets/Models"
    ENG_TEST_GLTF_VALIDATOR="${ENG_GLTF_VALIDATOR}"
    ENG_TEST_CONFORMANCE_OUT="${PROJECT_SOURCE_DIR}/build/f03/conformance.json")
if(MSVC)
    target_compile_options(asset_unit_tests PRIVATE /W4 /WX /permissive- /utf-8 /external:W0)
endif()
foreach(suite loader missing paths bad conformance decoders)
    add_test(NAME unit.asset.${suite} COMMAND asset_unit_tests --test-suite=asset.${suite} --no-version=true)
    set_tests_properties(unit.asset.${suite} PROPERTIES FAIL_REGULAR_EXPRESSION "test cases: +0 \\|")
endforeach()
set_tests_properties(unit.asset.conformance unit.asset.decoders PROPERTIES TIMEOUT 1200)
if(ENG_ASAN)
    # Every Sample-Assets decoder variant under ASan: ~980 s Debug on the dev machine (review of f03.t03), too close to
    # 1200 s; 3600 s leaves a 3.5x margin.
    set_tests_properties(unit.asset.conformance unit.asset.decoders PROPERTIES TIMEOUT 3600)
endif()
if(ENG_ASAN AND MSVC)
    # The MSVC ASan runtime DLL (clang_rt.asan_dynamic-x86_64.dll) sits next to cl.exe.
    get_filename_component(_eng_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    foreach(suite loader missing paths bad conformance decoders)
        set_tests_properties(unit.asset.${suite} PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_eng_cl_dir}")
    endforeach()
endif()

# --- shaderc (F1) ------------------------------------------------------------------------------
# Separate binary: it needs the offline toolchain. Missing tool -> exit 77 -> ctest "Skipped" with
# the reason printed (see tests/unit/shaderc/test_main.cpp).
if(ENG_BUILD_SHADERC)
    add_executable(shaderc_unit_tests
        shaderc/test_main.cpp
        shaderc/ShaderTestTools.h
        shaderc/test_targets.cpp
        shaderc/test_reflection.cpp
        shaderc/test_pbr.cpp)
    target_link_libraries(shaderc_unit_tests PRIVATE shaderc_core nlohmann_json::nlohmann_json doctest::doctest)
    target_compile_definitions(shaderc_unit_tests PRIVATE
        ENG_TEST_DXC="${ENG_DXC}"
        ENG_TEST_FXC="${ENG_FXC}"
        ENG_TEST_TINT="${ENG_TINT}"
        ENG_TEST_SPIRV_VAL="${ENG_SPIRV_VAL}"
        ENG_TEST_GLSLANG="${ENG_GLSLANG}"
        ENG_TEST_SHADER_DIR="${PROJECT_SOURCE_DIR}/shaders")
    if(MSVC)
        target_compile_options(shaderc_unit_tests PRIVATE /W4 /WX /permissive- /utf-8 /external:W0)
    endif()
    add_test(NAME unit.shaderc COMMAND shaderc_unit_tests --test-suite=shaderc --no-version=true)
    set_tests_properties(unit.shaderc PROPERTIES
        SKIP_RETURN_CODE 77
        FAIL_REGULAR_EXPRESSION "test cases: +0 \\|")

    # CLI exit codes (0 ok / 1 failure / 2 usage or tool missing), outputs under a non-ASCII path.
    set(sc_script "${PROJECT_SOURCE_DIR}/tests/golden/cli_exit_code.cmake")
    set(sc_src "${PROJECT_SOURCE_DIR}/shaders/test/triangle.hlsl")
    set(sc_have_tools TRUE)
    foreach(v ENG_DXC ENG_FXC ENG_TINT ENG_SPIRV_VAL ENG_GLSLANG)
        if(NOT ${v})
            set(sc_have_tools FALSE)
        endif()
    endforeach()
    function(shaderc_cli_test name expect)
        cmake_parse_arguments(PARSE_ARGV 2 T "" "EXPECT_IN_OUTPUT;EXPECT_FILE;UTF8_SRC" "ARGS")
        if(NOT sc_have_tools)
            # Same skip path as the unit tests: prints the missing tool, exit 77 -> ctest "Skipped".
            add_test(NAME shaderc.cli.${name} COMMAND shaderc_unit_tests)
            set_tests_properties(shaderc.cli.${name} PROPERTIES SKIP_RETURN_CODE 77)
            return()
        endif()
        set(defs -DTOOL=$<TARGET_FILE:shaderc> -DEXPECT=${expect} -DWORK=${CMAKE_CURRENT_BINARY_DIR}/shaderc_cli/${name})
        if(T_EXPECT_IN_OUTPUT)
            list(APPEND defs "-DEXPECT_IN_OUTPUT=${T_EXPECT_IN_OUTPUT}")
        endif()
        if(T_EXPECT_FILE)
            list(APPEND defs "-DEXPECT_FILE=${T_EXPECT_FILE}")
        endif()
        if(T_UTF8_SRC) # copied to @UTF8DIR@ (WORK/"ç ğ ü test") under its own file name
            get_filename_component(sc_name "${T_UTF8_SRC}" NAME)
            list(APPEND defs "-DUTF8_FIXTURE=${T_UTF8_SRC}" "-DUTF8_FIXTURE_NAME=${sc_name}")
        endif()
        string(REPLACE ";" "|" args "${T_ARGS}")
        add_test(NAME shaderc.cli.${name} COMMAND ${CMAKE_COMMAND} ${defs} "-DARGS=${args}" -P ${sc_script})
    endfunction()

    shaderc_cli_test(all_targets_vs 0 EXPECT_FILE "@WORK@/ç ğ ü out/triangle.VSMain.wgsl.wgsl"
        ARGS compile --src ${sc_src} --entry VSMain --stage vs --target all --out "@WORK@/ç ğ ü out" --validate)
    shaderc_cli_test(all_targets_fs 0 EXPECT_FILE "@WORK@/ç ğ ü out/triangle.PSMain.glsl-es300.refl.json"
        ARGS compile --src ${sc_src} --entry PSMain --stage fs --target all --out "@WORK@/ç ğ ü out" --validate)
    # Review Focus 5: non-ASCII + spaces in BOTH --src and --out.
    shaderc_cli_test(utf8_src_and_out 0 UTF8_SRC ${sc_src}
        EXPECT_FILE "@UTF8DIR@/çıktı ğ/triangle.PSMain.dxbc.dxbc"
        ARGS compile --src "@UTF8DIR@/triangle.hlsl" --entry PSMain --stage fs --target all
             --out "@UTF8DIR@/çıktı ğ" --validate)
    shaderc_cli_test(utf8_src_dxbc_direct 0 UTF8_SRC ${sc_src}
        EXPECT_FILE "@UTF8DIR@/out/triangle.VSMain.dxbc.dxbc"
        ARGS compile --src "@UTF8DIR@/triangle.hlsl" --entry VSMain --stage vs --target dxbc --dxbc-path direct
             --out "@UTF8DIR@/out" --validate)
    shaderc_cli_test(dxbc_direct 0 EXPECT_FILE "@WORK@/out/triangle.PSMain.dxbc.dxbc"
        ARGS compile --src ${sc_src} --entry PSMain --stage fs --target dxbc --dxbc-path direct --out @WORK@/out --validate)
    shaderc_cli_test(tool_missing 2 EXPECT_IN_OUTPUT "tool not found: spirv-val (run scripts/fetch_shader_tools.py)"
        ARGS compile --src ${sc_src} --entry VSMain --stage vs --target spirv --out @WORK@/out --validate
             --tool spirv-val=@WORK@/no-such-spirv-val.exe)
    shaderc_cli_test(usage 2 EXPECT_IN_OUTPUT "are required"
        ARGS compile --src ${sc_src} --entry VSMain --stage vs --target spirv)
    shaderc_cli_test(bad_target 2 EXPECT_IN_OUTPUT "unknown target"
        ARGS compile --src ${sc_src} --entry VSMain --stage vs --target hlsl --out @WORK@/out)
    # Review Focus 1 + 2: convention violation / unsupported feature -> exit 1 with the error text.
    shaderc_cli_test(binding_collision 1 EXPECT_IN_OUTPUT "binding collision in space2: b0 and t0"
        ARGS compile --src ${PROJECT_SOURCE_DIR}/shaders/test/bindings_collide.hlsl --entry PSMain --stage fs
             --target all --out @WORK@/out)
    shaderc_cli_test(unsupported_feature 1
        EXPECT_IN_OUTPUT "target glsl-es300 does not support storage buffers (TIER=1)"
        ARGS compile --src ${PROJECT_SOURCE_DIR}/shaders/test/needs_storage.hlsl --entry VSMain --stage vs
             --target glsl-es300 --define TIER=1 --out @WORK@/out)
    shaderc_cli_test(compile_error 1 EXPECT_IN_OUTPUT "NoSuchEntry"
        ARGS compile --src ${sc_src} --entry NoSuchEntry --stage vs --target spirv --out @WORK@/out)

    # Plan F1 Task 5: the committed MSL under tests/shaders/msl (validated by the macOS CI job with
    # `xcrun metal`) must be byte-identical to what this shaderc produces; refresh with
    # `python scripts/msl_artifacts.py`.
    find_package(Python3 COMPONENTS Interpreter)
    if(NOT sc_have_tools)
        add_test(NAME shaderc.msl_artifacts_fresh COMMAND shaderc_unit_tests)
        set_tests_properties(shaderc.msl_artifacts_fresh PROPERTIES SKIP_RETURN_CODE 77)
    elseif(Python3_Interpreter_FOUND)
        add_test(NAME shaderc.msl_artifacts_fresh
                 COMMAND Python3::Interpreter ${PROJECT_SOURCE_DIR}/scripts/msl_artifacts.py --check
                         --shaderc $<TARGET_FILE:shaderc>)
    else()
        message(WARNING "Python 3 not found: ctest shaderc.msl_artifacts_fresh is not registered")
    endif()
endif()

# --- RHI (F2) ----------------------------------------------------------------------------------
# rhi_unit_tests needs the shader library of the `shaders` build step (built first). One ctest entry
# per backend configuration; a backend that is compiled in but absent on the machine reports ctest
# "Skipped" with the reason (exit 77), a missing validation layer is a failure (tests/unit/rhi/test_main.cpp).
if(TARGET shaders)
    add_executable(rhi_unit_tests
        rhi/test_main.cpp
        rhi/RhiTestEnv.h
        rhi/test_shader_library.cpp
        rhi/test_triangle.cpp
        rhi/test_bind_group.cpp
        rhi/test_cube.cpp
        rhi/test_d3d11.cpp
        rhi/test_cross_backend.cpp
        rhi/test_pbr_spheres.cpp
        rhi/test_caps.cpp
        rhi/test_texture_upload.cpp
        rhi/SceneProbes.h)
    target_link_libraries(rhi_unit_tests PRIVATE engine::rhi engine::renderer golden_core nlohmann_json::nlohmann_json doctest::doctest
    eng_bcdec)
    if(ENG_RHI_VULKAN)
        # test_caps.cpp calls Vulkan through volk (the loaded entry points live in engine_rhi, VkLibs.cpp).
        target_link_libraries(rhi_unit_tests PRIVATE eng_volk)
    endif()
    target_compile_definitions(rhi_unit_tests PRIVATE ENG_TEST_SHADER_LIB_DIR="${ENG_SHADER_LIB_DIR}"
        ENG_TEST_ASSET_DIR="${PROJECT_SOURCE_DIR}/tests/assets")
    add_dependencies(rhi_unit_tests shaders)
    if(MSVC)
        target_compile_options(rhi_unit_tests PRIVATE /W4 /WX /permissive- /utf-8 /external:W0)
    endif()
    add_test(NAME unit.rhi.shaderlib COMMAND rhi_unit_tests --no-version=true)
    set(rhi_backends)
    if(ENG_RHI_VULKAN)
        list(APPEND rhi_backends vulkan)
    endif()
    if(ENG_RHI_D3D11)
        # d3d11-fl10_0 / d3d11-warp-fl10_0: D3D11 capped at FL10_0 = T0 (plan F2 Task 7, suite rhi.t0).
        list(APPEND rhi_backends d3d11 d3d11-warp d3d11-fl10_0 d3d11-warp-fl10_0)
    endif()
    foreach(b IN LISTS rhi_backends)
        add_test(NAME unit.rhi.${b} COMMAND rhi_unit_tests --rhi-backend=${b} --no-version=true)
        set_tests_properties(unit.rhi.${b} PROPERTIES
            SKIP_RETURN_CODE 77
            FAIL_REGULAR_EXPRESSION "test cases: +0 \\|")
    endforeach()
    set_tests_properties(unit.rhi.shaderlib PROPERTIES FAIL_REGULAR_EXPRESSION "test cases: +0 \\|")
    # ENG_REQUIRE_RHI_BACKENDS (plan F3 Task 1): every required backend has its ctest entry.
    foreach(b IN LISTS ENG_REQUIRE_RHI_BACKENDS)
        if(NOT TEST unit.rhi.${b})
            message(FATAL_ERROR "ENG_REQUIRE_RHI_BACKENDS lists '${b}' but ctest entry unit.rhi.${b} is not registered")
        endif()
    endforeach()
    # Plan F2 Task 4: D3D11 (hardware + WARP) vs Vulkan image equality; needs every backend (Skipped otherwise).
    if(ENG_RHI_VULKAN AND ENG_RHI_D3D11)
        add_test(NAME unit.rhi.cross COMMAND rhi_unit_tests --rhi-backend=cross --no-version=true)
        set_tests_properties(unit.rhi.cross PROPERTIES
            SKIP_RETURN_CODE 77
            FAIL_REGULAR_EXPRESSION "test cases: +0 \\|")
    endif()
endif()


# --- renderer facade (F3 Task 4) -----------------------------------------------------------------
# renderer_unit_tests links only engine::facade (include/engine/*.h, spec K-D) + nlohmann/json + doctest: no
# engine-internal include path. One ctest entry per backend; an absent backend is exit 77 = ctest Skipped.
if(TARGET shaders)
    add_executable(renderer_unit_tests renderer/test_facade.cpp)
    target_link_libraries(renderer_unit_tests PRIVATE engine::facade nlohmann_json::nlohmann_json doctest::doctest)
    target_compile_definitions(renderer_unit_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT
        ENG_TEST_SHADER_LIB_DIR="${ENG_SHADER_LIB_DIR}"
        ENG_TEST_ASSET_DIR="${PROJECT_SOURCE_DIR}/tests/assets"
        ENG_TEST_SAMPLE_MODELS="${PROJECT_SOURCE_DIR}/tests/assets/glTF-Sample-Assets/Models")
    add_dependencies(renderer_unit_tests shaders)
    if(MSVC)
        target_compile_options(renderer_unit_tests PRIVATE /W4 /WX /permissive- /utf-8 /external:W0)
    endif()
    set(renderer_backends ${ENG_BUILT_RHI_BACKENDS})
    if(ENG_RHI_D3D11)
        list(APPEND renderer_backends d3d11-fl10_0) # T0: SM 4.0, skin cbuffer, morph vertex attributes
    endif()
    foreach(b IN LISTS renderer_backends)
        add_test(NAME unit.renderer.${b} COMMAND renderer_unit_tests --backend=${b} --no-version=true)
        set_tests_properties(unit.renderer.${b} PROPERTIES
            SKIP_RETURN_CODE 77
            FAIL_REGULAR_EXPRESSION "test cases: +0 \\|")
    endforeach()
endif()
