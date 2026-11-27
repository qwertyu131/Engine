add_executable(engine_unit_tests
    test_main.cpp
    core/test_log.cpp
    core/test_cvar.cpp)

target_link_libraries(engine_unit_tests PRIVATE engine::engine doctest::doctest)

# One CTest entry per doctest suite, so `ctest -R <suite>` selects real test cases.
set(ENG_UNIT_SUITES log cvar)
foreach(suite IN LISTS ENG_UNIT_SUITES)
    add_test(NAME unit.${suite}
             COMMAND engine_unit_tests --test-suite=${suite} --no-version=true)
    # doctest exits 0 when a filter matches nothing; treat an empty suite as a failure.
    set_tests_properties(unit.${suite} PROPERTIES
        FAIL_REGULAR_EXPRESSION "test cases: +0 \\|")
endforeach()

# Catch-all: every case in the binary (guards against cases outside a listed suite).
add_test(NAME unit.all COMMAND engine_unit_tests --no-version=true)
