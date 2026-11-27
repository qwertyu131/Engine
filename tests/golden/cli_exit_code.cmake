# Runs the golden CLI and checks its exit code (and optionally a results.json substring).
# Usage: cmake -DGOLDEN=<exe> -DEXPECT=<code> -DARGS="a|b|c" [-DWORK=<dir>] [-DUTF8_FIXTURE=<png>]
#              [-DEXPECT_IN_RESULTS=<substring>] -P cli_exit_code.cmake
# ARGS may contain @WORK@ (a scratch dir recreated on every run) and @UTF8DIR@ (WORK/"ç ğ ü test",
# into which UTF8_FIXTURE is copied as red.png - or as UTF8_FIXTURE_NAME - to exercise non-ASCII
# paths with spaces).
# EXPECT_IN_RESULTS is searched in WORK/out/results.json.
# Optional: -DTOOL=<exe> runs another CLI (e.g. headless) instead of GOLDEN;
# -DEXPECT_IN_OUTPUT=<substring> must appear in stdout or stderr;
# -DEXPECT_FILE=<path> (may contain @WORK@ / @UTF8DIR@) must exist after the run.
cmake_minimum_required(VERSION 3.28)

string(REPLACE "|" ";" ARGS "${ARGS}")
if(DEFINED TOOL)
    set(GOLDEN "${TOOL}")
endif()

if(DEFINED WORK)
    file(REMOVE_RECURSE "${WORK}")
    file(MAKE_DIRECTORY "${WORK}")
    set(utf8dir "${WORK}/ç ğ ü test")
    if(DEFINED UTF8_FIXTURE)
        file(MAKE_DIRECTORY "${utf8dir}")
        if(NOT DEFINED UTF8_FIXTURE_NAME)
            set(UTF8_FIXTURE_NAME "red.png")
        endif()
        file(COPY_FILE "${UTF8_FIXTURE}" "${utf8dir}/${UTF8_FIXTURE_NAME}")
    endif()
    string(REPLACE "@UTF8DIR@" "${utf8dir}" ARGS "${ARGS}")
    string(REPLACE "@WORK@" "${WORK}" ARGS "${ARGS}")
    if(DEFINED EXPECT_FILE)
        string(REPLACE "@UTF8DIR@" "${utf8dir}" EXPECT_FILE "${EXPECT_FILE}")
        string(REPLACE "@WORK@" "${WORK}" EXPECT_FILE "${EXPECT_FILE}")
    endif()
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

if(DEFINED EXPECT_IN_OUTPUT)
    string(FIND "${out}${err}" "${EXPECT_IN_OUTPUT}" pos)
    if(pos EQUAL -1)
        message(FATAL_ERROR "output does not contain '${EXPECT_IN_OUTPUT}'")
    endif()
endif()

if(DEFINED EXPECT_FILE AND NOT EXISTS "${EXPECT_FILE}")
    message(FATAL_ERROR "expected file was not written: ${EXPECT_FILE}")
endif()
