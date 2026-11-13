# Fork: public package builds must not embed the builder's source or build paths.
# Keep ordinary/test builds unchanged: Rust tests use their manifest path for fixtures.
option(SATDUMP_PACKAGE_PATH_MAP "Fork: use neutral source/build paths in public packages" OFF)
if(NOT SATDUMP_PACKAGE_PATH_MAP)
    return()
endif()
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "SATDUMP_PACKAGE_PATH_MAP is for Linux packages")
endif()
set(SATDUMP_PACKAGE_SOURCE_ROOT "/usr/src/satdump-fork")
set(SATDUMP_PACKAGE_BUILD_ROOT "${SATDUMP_PACKAGE_SOURCE_ROOT}/build")
# -ffile-prefix-map covers both DWARF and __FILE__/source_location diagnostics.
# It changes filenames, not optimisation. Put the nested build mapping last so it wins.
add_compile_options(
    "-ffile-prefix-map=${CMAKE_SOURCE_DIR}=${SATDUMP_PACKAGE_SOURCE_ROOT}"
    "-ffile-prefix-map=${CMAKE_BINARY_DIR}=${SATDUMP_PACKAGE_BUILD_ROOT}")
