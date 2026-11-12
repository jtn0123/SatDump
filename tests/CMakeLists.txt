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

# Only a build with an install RPATH (the fork presets) is meant to run without LD_LIBRARY_PATH
if(CMAKE_INSTALL_RPATH)
    add_test(NAME cli_install_layout COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/cli/install_layout.sh ${SATDUMP_CHECK_PREFIX})
    set_tests_properties(cli_install_layout PROPERTIES LABELS cli)
endif()
