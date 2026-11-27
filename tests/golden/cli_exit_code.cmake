# Runs the golden CLI and checks its exit code (and optionally a results.json substring).
# Usage: cmake -DGOLDEN=<exe> -DEXPECT=<code> -DARGS="a|b|c" [-DWORK=<dir>] [-DUTF8_FIXTURE=<png>]
#              [-DEXPECT_IN_RESULTS=<substring>] -P cli_exit_code.cmake
# ARGS may contain @WORK@ (a scratch dir recreated on every run) and @UTF8DIR@ (WORK/"ç ğ ü test",
# into which UTF8_FIXTURE is copied as red.png, to exercise non-ASCII paths with spaces).
# EXPECT_IN_RESULTS is searched in WORK/out/results.json.
cmake_minimum_required(VERSION 3.28)

string(REPLACE "|" ";" ARGS "${ARGS}")

if(DEFINED WORK)
    file(REMOVE_RECURSE "${WORK}")
    file(MAKE_DIRECTORY "${WORK}")
    set(utf8dir "${WORK}/ç ğ ü test")
    if(DEFINED UTF8_FIXTURE)
        file(MAKE_DIRECTORY "${utf8dir}")
        file(COPY_FILE "${UTF8_FIXTURE}" "${utf8dir}/red.png")
    endif()
    string(REPLACE "@UTF8DIR@" "${utf8dir}" ARGS "${ARGS}")
    string(REPLACE "@WORK@" "${WORK}" ARGS "${ARGS}")
endif()

execute_process(COMMAND "${GOLDEN}" ${ARGS}
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
message(STATUS "golden ${ARGS}\n  exit=${rc}\n  stdout: ${out}\n  stderr: ${err}")
if(NOT rc STREQUAL EXPECT)
    message(FATAL_ERROR "expected exit ${EXPECT}, got ${rc}")
endif()

if(DEFINED EXPECT_IN_RESULTS)
    file(READ "${WORK}/out/results.json" results)
    string(FIND "${results}" "${EXPECT_IN_RESULTS}" pos)
    if(pos EQUAL -1)
        message(FATAL_ERROR "results.json does not contain '${EXPECT_IN_RESULTS}':\n${results}")
    endif()
endif()
