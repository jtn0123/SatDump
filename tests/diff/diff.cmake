# Fork: differential tests for the Rust parts (tests/diff/diff.h): every twin's C++ against its
# Rust on seeded, edge and corpus cases; JSONL oracles that cargo test replays without the C++;
# reproducers in <build>/diff-failures. Included by tests/CMakeLists.txt in Rust builds only,
# so its tests get the same sanitizer settings as the rest. Nothing here is installed.
set(_diff ${CMAKE_CURRENT_LIST_DIR})

# The float code of the self-test twins, compiled twice (floats.cpp): with -ffp-contract=off,
# as diff.h's float rule 1 has ported production sources compiled in every build, and with the
# project's own flags, to show what that rule is for. A twin's C++ side is otherwise always
# the production code as the build compiles it, never a copy compiled with other flags.
add_library(satdump_diff_floats_strict OBJECT ${_diff}/floats.cpp)
target_compile_options(satdump_diff_floats_strict PRIVATE -ffp-contract=off)
target_compile_definitions(satdump_diff_floats_strict PRIVATE SD_FLOATS_BUILD=strict)
add_library(satdump_diff_floats_contracted OBJECT ${_diff}/floats.cpp)
target_compile_definitions(satdump_diff_floats_contracted PRIVATE SD_FLOATS_BUILD=contracted)
foreach(_t satdump_diff_floats_strict satdump_diff_floats_contracted)
    target_include_directories(${_t} PRIVATE ${_diff} ${PROJECT_SOURCE_DIR}/src-core)
endforeach()

add_library(satdump_diff STATIC
    ${_diff}/cases.cpp
    ${_diff}/outcome.cpp
    ${_diff}/io.cpp
    ${_diff}/run.cpp
    ${_diff}/twins_derand.cpp
    ${_diff}/twins_selftest.cpp
    $<TARGET_OBJECTS:satdump_diff_floats_strict>
    $<TARGET_OBJECTS:satdump_diff_floats_contracted>)
target_include_directories(satdump_diff PUBLIC ${_diff} ${PROJECT_SOURCE_DIR}/src-core)
target_link_libraries(satdump_diff PUBLIC satdump_core)
# Where reproducers go, and what the replay commands written with them run
target_compile_definitions(satdump_diff PRIVATE
    SATDUMP_DIFF_FAILURES="${CMAKE_BINARY_DIR}/diff-failures"
    SATDUMP_DIFF_TOOL="$<TARGET_FILE:satdump-diff>"
    SATDUMP_DIFF_CARGO="${SATDUMP_CARGO}"
    SATDUMP_DIFF_CARGO_TOML="${PROJECT_SOURCE_DIR}/rust/Cargo.toml"
    SATDUMP_DIFF_CARGO_TARGET="${SATDUMP_RUST_TARGET_DIR}")

add_executable(satdump-diff ${_diff}/main.cpp)
target_link_libraries(satdump-diff PRIVATE satdump_diff)

add_executable(satdump-diff-tests ${CMAKE_CURRENT_SOURCE_DIR}/unit/main.cpp ${_diff}/test_diff.cpp ${_diff}/test_diff_rules.cpp)
target_include_directories(satdump-diff-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/doctest)
target_link_libraries(satdump-diff-tests PRIVATE satdump_diff)

# The framework's own tests, then every twin with the default plan (SD_DIFF_CASES seeded cases)
add_test(NAME diff_selftest COMMAND satdump-diff-tests)
add_test(NAME diff_twins COMMAND satdump-diff run)
# The committed oracles still match the C++ (else write them again: satdump-diff oracle <twin>
# --small --out tests/diff/oracles/<twin>.jsonl) and Rust matches them
add_test(NAME diff_oracles COMMAND satdump-diff check --min-cases 6 ${_diff}/oracles)
set_tests_properties(diff_selftest diff_twins diff_oracles PROPERTIES LABELS unit)

if(Python3_FOUND)
    # The golden test recordings, one CADU at a time
    add_test(NAME diff_corpus_make
        COMMAND ${Python3_EXECUTABLE} -I -B ${CMAKE_CURRENT_SOURCE_DIR}/golden/make_recordings.py ${CMAKE_BINARY_DIR}/diff-corpus)
    add_test(NAME diff_corpus COMMAND satdump-diff run
        --corpus ${CMAKE_BINARY_DIR}/diff-corpus/metop.cadu --corpus ${CMAKE_BINARY_DIR}/diff-corpus/meteor.cadu)
    set_tests_properties(diff_corpus_make PROPERTIES LABELS unit FIXTURES_SETUP diff_corpus)
    set_tests_properties(diff_corpus PROPERTIES LABELS unit FIXTURES_REQUIRED diff_corpus)
endif()

# Rust against oracles of the full default plan written by this build's C++, without the C++
# (rust/tests/difftest/main.rs reads SD_DIFF_ORACLES)
set(_diff_oracles ${CMAKE_BINARY_DIR}/diff-oracles)
add_test(NAME diff_oracles_make COMMAND satdump-diff oracle all --out ${_diff_oracles})
add_test(NAME diff_rust_replay COMMAND ${CMAKE_COMMAND} -E env CARGO_TARGET_DIR=${SATDUMP_RUST_TARGET_DIR}
    SD_DIFF_ORACLES=${_diff_oracles} SD_DIFF_FAILURES=${CMAKE_BINARY_DIR}/diff-failures SD_DIFF_TOOL=$<TARGET_FILE:satdump-diff>
    ${SATDUMP_CARGO} test --release --offline --locked --manifest-path ${PROJECT_SOURCE_DIR}/rust/Cargo.toml
    --test difftest)
set_tests_properties(diff_oracles_make PROPERTIES LABELS unit FIXTURES_SETUP diff_oracles)
set_tests_properties(diff_rust_replay PROPERTIES LABELS unit FIXTURES_REQUIRED diff_oracles RESOURCE_LOCK cargo)
