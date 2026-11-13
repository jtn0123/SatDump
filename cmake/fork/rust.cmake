# Fork: SatDump's Rust parts (rust/), off by default. Included by src-core/CMakeLists.txt.
#
# -DSATDUMP_WITH_RUST=ON builds the crate with cargo (offline; it has no dependencies) and
# links it into satdump_core. The Rust symbols stay inside libsatdump_core.so
# (--exclude-libs), so plugins reach Rust only through the C++ functions in rust/bridge.
# At run time SATDUMP_NO_RUST=1 switches back to the C++ code.
option(SATDUMP_WITH_RUST "Fork: build SatDump's Rust parts (rust/)" OFF)
if(NOT SATDUMP_WITH_RUST)
    return()
endif()

find_program(SATDUMP_CARGO cargo REQUIRED)
set(SATDUMP_RUST_DIR ${PROJECT_SOURCE_DIR}/rust)
set(SATDUMP_RUST_TARGET_DIR ${PROJECT_BINARY_DIR}/rust-target CACHE INTERNAL "cargo build folder (tests use it too)")
set(_satdump_rs_lib ${SATDUMP_RUST_TARGET_DIR}/release/libsatdump_rs.a)
file(GLOB _satdump_rs_sources CONFIGURE_DEPENDS ${SATDUMP_RUST_DIR}/src/*.rs)
# Fork part "aos": the CCSDS AOS demuxer's Rust files (rust/src/aos)
file(GLOB _satdump_rs_aos_sources CONFIGURE_DEPENDS ${SATDUMP_RUST_DIR}/src/aos/*.rs)
list(APPEND _satdump_rs_sources ${_satdump_rs_aos_sources})
# LRPT (M6.3): the segment decoder's own folder
file(GLOB _satdump_rs_lrpt CONFIGURE_DEPENDS ${SATDUMP_RUST_DIR}/src/lrpt/*.rs)
list(APPEND _satdump_rs_sources ${_satdump_rs_lrpt})

set(_satdump_rs_env "")
if(SATDUMP_PACKAGE_PATH_MAP)
    # Preserve caller flags; encoded flags take precedence over RUSTFLAGS in cargo.
    string(ASCII 31 _satdump_rs_sep)
    if(DEFINED ENV{CARGO_ENCODED_RUSTFLAGS})
        set(_satdump_rs_flags "$ENV{CARGO_ENCODED_RUSTFLAGS}")
    else()
        separate_arguments(_satdump_rs_flags UNIX_COMMAND "$ENV{RUSTFLAGS}")
        list(JOIN _satdump_rs_flags "${_satdump_rs_sep}" _satdump_rs_flags)
    endif()
    foreach(_satdump_rs_map
            "${CMAKE_SOURCE_DIR}=${SATDUMP_PACKAGE_SOURCE_ROOT}"
            "${CMAKE_BINARY_DIR}=${SATDUMP_PACKAGE_BUILD_ROOT}"
            "${SATDUMP_RUST_TARGET_DIR}=${SATDUMP_PACKAGE_BUILD_ROOT}/rust-target")
        if(_satdump_rs_flags)
            string(APPEND _satdump_rs_flags "${_satdump_rs_sep}")
        endif()
        string(APPEND _satdump_rs_flags "--remap-path-prefix=${_satdump_rs_map}")
    endforeach()
    list(APPEND _satdump_rs_env "CARGO_ENCODED_RUSTFLAGS=${_satdump_rs_flags}")
endif()

add_custom_command(OUTPUT ${_satdump_rs_lib}
    COMMAND ${CMAKE_COMMAND} -E env CARGO_TARGET_DIR=${SATDUMP_RUST_TARGET_DIR} ${_satdump_rs_env}
            ${SATDUMP_CARGO} build --release --offline --locked --manifest-path ${SATDUMP_RUST_DIR}/Cargo.toml
    DEPENDS ${_satdump_rs_sources} ${SATDUMP_RUST_DIR}/Cargo.toml ${SATDUMP_RUST_DIR}/Cargo.lock
    COMMENT "Building SatDump's Rust parts (cargo)"
    VERBATIM)
add_custom_target(satdump_rs_build DEPENDS ${_satdump_rs_lib})
add_dependencies(satdump_core satdump_rs_build)

target_sources(satdump_core PRIVATE ${SATDUMP_RUST_DIR}/bridge/rust_bridge.cpp)
# Fork part "aos": the C++ side of the Rust AOS demuxer (used by ccsds_aos/demuxer.cpp)
target_sources(satdump_core PRIVATE ${SATDUMP_RUST_DIR}/bridge/rust_aos.cpp)
target_include_directories(satdump_core PUBLIC ${SATDUMP_RUST_DIR}/bridge)
target_compile_definitions(satdump_core PRIVATE SATDUMP_WITH_RUST=1)
target_link_libraries(satdump_core PRIVATE ${_satdump_rs_lib} ${CMAKE_DL_LIBS})
target_link_options(satdump_core PRIVATE "LINKER:--exclude-libs,libsatdump_rs.a")
# LRPT (M6.3): its bridge, and SATDUMP_WITH_RUST for the code that links satdump_core too, so
# the Meteor plugin's Segment (and the tests that compile it) can call it. Rust builds only:
# Rust-off builds keep the compile flags they had.
target_sources(satdump_core PRIVATE ${SATDUMP_RUST_DIR}/bridge/rust_lrpt.cpp)
target_compile_definitions(satdump_core INTERFACE SATDUMP_WITH_RUST=1)
message("-- Rust parts: on (${SATDUMP_CARGO})")
