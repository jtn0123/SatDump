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

add_custom_command(OUTPUT ${_satdump_rs_lib}
    COMMAND ${CMAKE_COMMAND} -E env CARGO_TARGET_DIR=${SATDUMP_RUST_TARGET_DIR}
            ${SATDUMP_CARGO} build --release --offline --locked --manifest-path ${SATDUMP_RUST_DIR}/Cargo.toml
    DEPENDS ${_satdump_rs_sources} ${SATDUMP_RUST_DIR}/Cargo.toml ${SATDUMP_RUST_DIR}/Cargo.lock
    COMMENT "Building SatDump's Rust parts (cargo)"
    VERBATIM)
add_custom_target(satdump_rs_build DEPENDS ${_satdump_rs_lib})
add_dependencies(satdump_core satdump_rs_build)

target_sources(satdump_core PRIVATE ${SATDUMP_RUST_DIR}/bridge/rust_bridge.cpp)
target_include_directories(satdump_core PUBLIC ${SATDUMP_RUST_DIR}/bridge)
target_compile_definitions(satdump_core PRIVATE SATDUMP_WITH_RUST=1)
target_link_libraries(satdump_core PRIVATE ${_satdump_rs_lib} ${CMAKE_DL_LIBS})
target_link_options(satdump_core PRIVATE "LINKER:--exclude-libs,libsatdump_rs.a")
message("-- Rust parts: on (${SATDUMP_CARGO})")
