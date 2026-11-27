add_executable(engine_unit_tests
    test_main.cpp
    core/test_log.cpp)

target_link_libraries(engine_unit_tests PRIVATE engine::engine doctest::doctest)

# One CTest entry per doctest suite, so `ctest -R <suite>` selects real test cases.
set(ENG_UNIT_SUITES log)
foreach(suite IN LISTS ENG_UNIT_SUITES)
    add_test(NAME unit.${suite}
             COMMAND engine_unit_tests --test-suite=${suite} --no-version=true)
endforeach()

# Catch-all: every case in the binary (guards against cases outside a listed suite).
add_test(NAME unit.all COMMAND engine_unit_tests --no-version=true)
