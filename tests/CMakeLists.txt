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
    unit/test_logger.cpp
    unit/test_logged_fallbacks.cpp
    unit/test_task_scheduler.cpp
    unit/test_aos_demuxer.cpp
    unit/test_metop_instruments.cpp
    unit/test_hirs_iasi.cpp
    unit/test_init_ub.cpp
    unit/test_block_lifetime.cpp
    unit/test_cc_decoder.cpp
    unit/test_temp_file.cpp
    unit/test_ggak_merger.cpp
    unit/test_msugs_vis.cpp
    unit/test_thread_faults.cpp
    unit/test_autotrack_faults.cpp
    unit/test_aec.cpp
    unit/test_j2k.cpp
    unit/test_j2k_stream.cpp
    unit/test_mqtt.cpp
    unit/test_mqtt_client.cpp
    # Plugin code under test is compiled in directly
    ${PROJECT_SOURCE_DIR}/plugins/noaa_metop_support/metop/instruments/admin_msg/admin_msg_reader.cpp
    ${PROJECT_SOURCE_DIR}/plugins/noaa_metop_support/instruments/amsu/amsu_reader.cpp
    ${PROJECT_SOURCE_DIR}/plugins/noaa_metop_support/instruments/mhs/mhs_reader.cpp
    ${PROJECT_SOURCE_DIR}/plugins/noaa_metop_support/metop/instruments/gome/gome_reader.cpp
    ${PROJECT_SOURCE_DIR}/plugins/noaa_metop_support/metop/instruments/iasi/iasi_imaging_reader.cpp
    ${PROJECT_SOURCE_DIR}/plugins/noaa_metop_support/noaa/instruments/hirs/hirs_reader.cpp
    ${PROJECT_SOURCE_DIR}/plugins/noaa_metop_support/noaa/contains.cpp
    ${PROJECT_SOURCE_DIR}/plugins/noaa_metop_support/metop/module_metop_ahrpt_decoder.cpp
    ${PROJECT_SOURCE_DIR}/plugins/noaa_metop_support/metop/module_metop_dump_decoder.cpp
    ${PROJECT_SOURCE_DIR}/plugins/meteor_support/meteor/module_meteor_lrpt_decoder.cpp
    ${PROJECT_SOURCE_DIR}/plugins/meteor_support/meteor/deint.cpp
    ${PROJECT_SOURCE_DIR}/plugins/meteor_support/meteor/instruments/msumr/lrpt/segment.cpp
    ${PROJECT_SOURCE_DIR}/plugins/meteor_support/meteor/instruments/msumr/lrpt/huffman.cpp
    ${PROJECT_SOURCE_DIR}/plugins/meteor_support/meteor/instruments/msumr/lrpt/idct.cpp
    ${PROJECT_SOURCE_DIR}/plugins/elektro_arktika_support/elektro_arktika/ggak/ggak.cpp
    ${PROJECT_SOURCE_DIR}/plugins/elektro_arktika_support/elektro_arktika/instruments/msugs/msu_vis_reader.cpp
)
target_include_directories(satdump-unit-tests PRIVATE
    third_party/doctest
    ${PROJECT_SOURCE_DIR}/src-core
    ${PROJECT_SOURCE_DIR}/plugins/noaa_metop_support
    ${PROJECT_SOURCE_DIR}/plugins/meteor_support
    ${PROJECT_SOURCE_DIR}/plugins/elektro_arktika_support
)
target_link_libraries(satdump-unit-tests PRIVATE satdump_core)
# Fork: libaec for test_aec.cpp (cmake/fork/codecs.cmake)
satdump_link_aec(satdump-unit-tests)
if(SATDUMP_WITH_RUST)
    target_sources(satdump-unit-tests PRIVATE unit/test_rust_bridge.cpp)
endif()
# Fork: composite expressions give the same results with Debian's muparser (Linux) as with the
# bundled copy, and the bundled 8-bit libjpeg's results are pinned; Linux builds link libmuparser
target_sources(satdump-unit-tests PRIVATE unit/test_composite_expressions.cpp unit/test_jpeg8.cpp)
target_compile_definitions(satdump-unit-tests PRIVATE SATDUMP_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_test(NAME cli_system_libs COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/cli/system_libs.sh ${SATDUMP_CHECK_PREFIX})
    set_tests_properties(cli_system_libs PROPERTIES LABELS cli)
endif()

add_test(NAME unit COMMAND satdump-unit-tests)
set_tests_properties(unit PROPERTIES LABELS unit)

find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME theme_lint
        COMMAND ${Python3_EXECUTABLE} -I -B ${PROJECT_SOURCE_DIR}/tools/theme_gen/gen.py --check ${PROJECT_SOURCE_DIR}/resources/themes)
    set_tests_properties(theme_lint PROPERTIES LABELS lint)
endif()

# Logger calls: every form the code base uses compiles cleanly, printf-style calls with
# arguments are checked by the compiler, and a format built at run time with arguments is refused
add_test(NAME logger_format_check COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/compile/logger_format.sh ${CMAKE_CXX_COMPILER} ${PROJECT_SOURCE_DIR}/src-core)
set_tests_properties(logger_format_check PROPERTIES LABELS lint)

add_test(NAME cli_exit_codes COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/cli/exit_codes.sh ${SATDUMP_CHECK_PREFIX})
set_tests_properties(cli_exit_codes PROPERTIES LABELS cli)

add_test(NAME cli_short_inputs COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/cli/short_inputs.sh ${SATDUMP_CHECK_PREFIX})
set_tests_properties(cli_short_inputs PROPERTIES LABELS cli)

add_test(NAME cli_startup COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/cli/startup.sh ${SATDUMP_CHECK_PREFIX})
set_tests_properties(cli_startup PROPERTIES LABELS cli)

# Modules that fail on purpose (tests/unit/fault_module.h), as a plugin for the cli test only;
# never installed
add_library(fork_fault_test_plugin SHARED unit/fault_plugin.cpp)
target_include_directories(fork_fault_test_plugin PRIVATE ${PROJECT_SOURCE_DIR}/src-core)
target_link_libraries(fork_fault_test_plugin PRIVATE satdump_core)
add_test(NAME cli_thread_faults
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/cli/thread_faults.sh ${SATDUMP_CHECK_PREFIX} $<TARGET_FILE:fork_fault_test_plugin>)
set_tests_properties(cli_thread_faults PROPERTIES LABELS cli)

if(Python3_FOUND)
    add_test(NAME thread_catches_lint COMMAND ${Python3_EXECUTABLE} -I -B ${CMAKE_CURRENT_SOURCE_DIR}/lint/thread_catches.py ${PROJECT_SOURCE_DIR})
    set_tests_properties(thread_catches_lint PROPERTIES LABELS lint)
    add_test(NAME ratchet_selftest COMMAND ${Python3_EXECUTABLE} -I -B ${CMAKE_CURRENT_SOURCE_DIR}/warnings/test_ratchet.py)
    set_tests_properties(ratchet_selftest PROPERTIES LABELS lint)
    # Fork: the bundled third-party libraries must match tests/deps/third_party.json (version,
    # contents, no unlisted library in the scanned folders) and DEPENDENCIES.md must show each
    # listed version; see DEPENDENCIES.md
    add_test(NAME deps_check COMMAND ${Python3_EXECUTABLE} -I -B ${CMAKE_CURRENT_SOURCE_DIR}/deps/check_deps.py ${PROJECT_SOURCE_DIR})
    set_tests_properties(deps_check PROPERTIES LABELS lint)
    add_test(NAME deps_check_selftest COMMAND ${Python3_EXECUTABLE} -I -B ${CMAKE_CURRENT_SOURCE_DIR}/deps/test_check_deps.py)
    set_tests_properties(deps_check_selftest PROPERTIES LABELS lint)
endif()

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

# Fork: no headers are installed for code the core does not build (cmake/fork/codecs.cmake)
add_test(NAME cli_install_headers COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/cli/install_headers.sh ${CMAKE_BINARY_DIR})
set_tests_properties(cli_install_headers PROPERTIES LABELS cli)

# Fork: the Debian package settings (name, generated dependencies, version), checked without
# building a package; tests/package/check_deb.sh checks a package built by scripts/build-deb.sh
# (label cli: it also checks where the gate installed the libraries and plugins)
find_program(SATDUMP_DPKG dpkg)
if(SATDUMP_DPKG AND ENABLE_INSTALL AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_test(NAME deb_settings
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/package/deb_settings.sh ${CMAKE_BINARY_DIR} ${PROJECT_SOURCE_DIR} ${SATDUMP_CHECK_PREFIX} ${CMAKE_COMMAND})
    set_tests_properties(deb_settings PROPERTIES LABELS cli)
endif()

# Rust builds (-DSATDUMP_WITH_RUST=ON): the crate's own tests, clippy with warnings as errors,
# the golden pictures once more with Rust switched off, and how the Rust code is linked
if(SATDUMP_WITH_RUST)
    set(_cargo ${CMAKE_COMMAND} -E env CARGO_TARGET_DIR=${SATDUMP_RUST_TARGET_DIR} ${SATDUMP_CARGO})
    set(_manifest --manifest-path ${PROJECT_SOURCE_DIR}/rust/Cargo.toml)
    add_test(NAME rust_unit COMMAND ${_cargo} test --release --offline --locked ${_manifest})
    add_test(NAME rust_clippy COMMAND ${_cargo} clippy --release --offline --locked --all-targets ${_manifest} -- -D warnings)
    set_tests_properties(rust_unit PROPERTIES LABELS unit RESOURCE_LOCK cargo)
    set_tests_properties(rust_clippy PROPERTIES LABELS lint RESOURCE_LOCK cargo)
    add_test(NAME cli_rust COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/cli/rust.sh ${SATDUMP_CHECK_PREFIX})
    set_tests_properties(cli_rust PROPERTIES LABELS cli)
    if(Python3_FOUND)
        add_test(NAME golden_no_rust COMMAND ${CMAKE_COMMAND} -E env SATDUMP_NO_RUST=1
            bash ${CMAKE_CURRENT_SOURCE_DIR}/golden/golden.sh ${SATDUMP_CHECK_PREFIX} ${_golden_mode})
        set_tests_properties(golden_no_rust PROPERTIES LABELS cli)
    endif()
endif()

if(SATDUMP_FUZZ)
    add_subdirectory(fuzz)
endif()

# Sanitizer builds (-DSATDUMP_SANITIZERS=address,undefined): any finding fails the test with
# exit code 86, memory leaks included (the few kept on purpose are listed in
# sanitizers/lsan.supp). Every plugin defines its own SATDUMP_ABI_VERSION, which ASan reports
# as a one-definition-rule violation when the plugins load; that one check is switched off.
if(SATDUMP_SANITIZERS)
    get_property(_satdump_tests DIRECTORY PROPERTY TESTS)
    set_tests_properties(${_satdump_tests} PROPERTIES ENVIRONMENT
        "ASAN_OPTIONS=detect_odr_violation=0:detect_leaks=1:exitcode=86;LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/sanitizers/lsan.supp:print_suppressions=0;UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1:exitcode=86")
endif()
