# Fork: tests. Configure with -DSATDUMP_TESTS=ON, then run tests/gate.sh (or ctest).
#
# Labels: unit (fast, no files), lint, cli (runs the installed program from
# SATDUMP_CHECK_PREFIX in an empty folder with its own HOME).

set(SATDUMP_CHECK_PREFIX "${CMAKE_BINARY_DIR}/check-prefix" CACHE PATH "Where gate.sh installs the build for the cli tests")

add_executable(satdump-unit-tests
    unit/main.cpp
    unit/test_randomization.cpp
    unit/test_reedsolomon.cpp
    unit/test_ccsds.cpp
    unit/test_admin_msg.cpp
    unit/test_filestream.cpp
    unit/test_image.cpp
    unit/test_task_scheduler.cpp
    # Plugin code under test is compiled in directly
    ${PROJECT_SOURCE_DIR}/plugins/noaa_metop_support/metop/instruments/admin_msg/admin_msg_reader.cpp
)
target_include_directories(satdump-unit-tests PRIVATE
    third_party/doctest
    ${PROJECT_SOURCE_DIR}/src-core
    ${PROJECT_SOURCE_DIR}/plugins/noaa_metop_support
)
target_link_libraries(satdump-unit-tests PRIVATE satdump_core)

add_test(NAME unit COMMAND satdump-unit-tests)
set_tests_properties(unit PROPERTIES LABELS unit)

find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME theme_lint
        COMMAND ${Python3_EXECUTABLE} -I -B ${PROJECT_SOURCE_DIR}/tools/theme_gen/gen.py --check ${PROJECT_SOURCE_DIR}/resources/themes)
    set_tests_properties(theme_lint PROPERTIES LABELS lint)
endif()

add_test(NAME cli_exit_codes COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/cli/exit_codes.sh ${SATDUMP_CHECK_PREFIX})
set_tests_properties(cli_exit_codes PROPERTIES LABELS cli)

add_test(NAME cli_short_inputs COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/cli/short_inputs.sh ${SATDUMP_CHECK_PREFIX})
set_tests_properties(cli_short_inputs PROPERTIES LABELS cli)

# Synthetic MetOp and NOAA recordings must give exactly the known-good pictures. Sanitizer
# builds compare only which pictures are made (their compiler settings differ).
if(Python3_FOUND)
    if(SATDUMP_SANITIZERS)
        set(_golden_mode --names-only)
    endif()
    add_test(NAME golden COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/golden/golden.sh ${SATDUMP_CHECK_PREFIX} ${_golden_mode})
    set_tests_properties(golden PROPERTIES LABELS cli)
endif()

# Only a relocatable build (the fork presets) is meant to run without LD_LIBRARY_PATH
if(SATDUMP_RELOCATABLE)
    add_test(NAME cli_install_layout COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/cli/install_layout.sh ${SATDUMP_CHECK_PREFIX})
    set_tests_properties(cli_install_layout PROPERTIES LABELS cli)
endif()

if(SATDUMP_FUZZ)
    add_subdirectory(fuzz)
endif()

# Sanitizer builds (-DSATDUMP_SANITIZERS=address,undefined): any finding fails the test with
# exit code 86. Every plugin defines its own SATDUMP_ABI_VERSION, which ASan reports as a
# one-definition-rule violation when the plugins load; that one check is switched off.
if(SATDUMP_SANITIZERS)
    get_property(_satdump_tests DIRECTORY PROPERTY TESTS)
    set_tests_properties(${_satdump_tests} PROPERTIES ENVIRONMENT
        "ASAN_OPTIONS=detect_odr_violation=0:detect_leaks=0:exitcode=86;UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1:exitcode=86")
endif()
