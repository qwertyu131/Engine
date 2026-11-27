# CLI exit-code tests for the golden tool (plan F0 Task 3 Step 4, Review Focus 1-2, 4).
set(fx "${PROJECT_SOURCE_DIR}/tests/golden/fixtures")
set(script "${CMAKE_CURRENT_SOURCE_DIR}/cli_exit_code.cmake")
set(work "${CMAKE_CURRENT_BINARY_DIR}/cli")

function(golden_cli_test name expect)
    cmake_parse_arguments(PARSE_ARGV 2 T "" "EXPECT_IN_RESULTS;UTF8_FIXTURE" "ARGS")
    set(defs -DGOLDEN=$<TARGET_FILE:golden> -DEXPECT=${expect} -DWORK=${work}/${name})
    if(T_EXPECT_IN_RESULTS)
        list(APPEND defs -DEXPECT_IN_RESULTS=${T_EXPECT_IN_RESULTS})
    endif()
    if(T_UTF8_FIXTURE)
        list(APPEND defs -DUTF8_FIXTURE=${T_UTF8_FIXTURE})
    endif()
    string(REPLACE ";" "|" args "${T_ARGS}") # "|"-separated; split again in the script
    add_test(NAME golden.cli.${name}
             COMMAND ${CMAKE_COMMAND} ${defs} "-DARGS=${args}" -P ${script})
endfunction()

golden_cli_test(identical 0 ARGS compare --test ${fx}/solid_red_64.png --ref ${fx}/solid_red_64_copy.png
                                       --out-diff @WORK@/diff.png --json @WORK@/result.json)
golden_cli_test(over_threshold 1 ARGS compare --test ${fx}/gray_64.png --ref ${fx}/solid_red_64.png
                                            --out-diff @WORK@/diff.png)
golden_cli_test(size_mismatch 2 ARGS compare --test ${fx}/solid_red_64.png --ref ${fx}/solid_red_32.png
                                           --out-diff @WORK@/diff.png)
golden_cli_test(channel_mismatch 2 ARGS compare --test ${fx}/solid_red_64.png --ref ${fx}/gray_64_l8.png
                                              --out-diff @WORK@/diff.png)
golden_cli_test(missing_input 2 ARGS compare --test ${fx}/solid_red_64.png --ref ${fx}/no_such_reference.png
                                           --out-diff @WORK@/diff.png)
golden_cli_test(bad_threshold 2 ARGS compare --test ${fx}/solid_red_64.png --ref ${fx}/solid_red_64_copy.png
                                           --out-diff @WORK@/diff.png --threshold-flip abc)
golden_cli_test(usage 2 ARGS compare --test ${fx}/solid_red_64.png)
golden_cli_test(utf8_paths 0 UTF8_FIXTURE ${fx}/solid_red_64.png
                ARGS compare --test @UTF8DIR@/red.png --ref ${fx}/solid_red_64_copy.png
                     --out-diff @UTF8DIR@/fark.png)
golden_cli_test(batch_pass 0 EXPECT_IN_RESULTS PASS
                ARGS batch --manifest ${fx}/batch_manifest_pass.json --out @WORK@/out)
golden_cli_test(batch_missing_reference 1 EXPECT_IN_RESULTS "missing-reference"
                ARGS batch --manifest ${fx}/batch_manifest.json --out @WORK@/out)
golden_cli_test(batch_bad_manifest 2 ARGS batch --manifest ${fx}/no_such_manifest.json --out @WORK@/out)

# headless CLI exit codes (plan F0 Task 5): 0 = ok, 2 = usage / IO, 3 = backend not available.
function(headless_cli_test name expect)
    cmake_parse_arguments(PARSE_ARGV 2 T "" "EXPECT_IN_OUTPUT;EXPECT_FILE" "ARGS")
    set(defs -DTOOL=$<TARGET_FILE:headless> -DEXPECT=${expect} -DWORK=${work}/headless_${name})
    if(T_EXPECT_IN_OUTPUT)
        list(APPEND defs "-DEXPECT_IN_OUTPUT=${T_EXPECT_IN_OUTPUT}")
    endif()
    if(T_EXPECT_FILE)
        list(APPEND defs "-DEXPECT_FILE=${T_EXPECT_FILE}")
    endif()
    string(REPLACE ";" "|" args "${T_ARGS}")
    add_test(NAME headless.cli.${name}
             COMMAND ${CMAKE_COMMAND} ${defs} "-DARGS=${args}" -P ${script})
endfunction()

set(tp --scene builtin:test-pattern --width 256 --height 256)
headless_cli_test(cpu_ok 0 EXPECT_FILE @WORK@/tp.png
                  ARGS --backend cpu ${tp} --out @WORK@/tp.png)
headless_cli_test(backend_unavailable 3 EXPECT_IN_OUTPUT "backend 'd3d12' not available in this build"
                  ARGS --backend d3d12 ${tp} --out @WORK@/tp.png)
headless_cli_test(bench 0 EXPECT_IN_OUTPUT "frame_ms_p95=" EXPECT_FILE @WORK@/tp.png
                  ARGS --backend cpu ${tp} --out @WORK@/tp.png --bench 5 --cvar r.Bench.WarmupFrames=2)
headless_cli_test(cvar_unknown 2 EXPECT_IN_OUTPUT "unknown cvar 'r.No.Such'"
                  ARGS --backend cpu ${tp} --out @WORK@/tp.png --cvar r.No.Such=1)
headless_cli_test(cvar_bad_value 2 EXPECT_IN_OUTPUT "rejected value"
                  ARGS --backend cpu ${tp} --out @WORK@/tp.png --cvar r.Bench.WarmupFrames=abc)
headless_cli_test(scene_unsupported 2 EXPECT_IN_OUTPUT "not supported"
                  ARGS --backend cpu --scene model.glb --out @WORK@/tp.png)
headless_cli_test(bad_size 2 ARGS --backend cpu ${tp} --width 0 --out @WORK@/tp.png)
headless_cli_test(usage 2 ARGS --backend cpu)
headless_cli_test(utf8_out 0 EXPECT_FILE "@UTF8DIR@/desen.png"
                  ARGS --backend cpu ${tp} --out "@UTF8DIR@/desen.png")
