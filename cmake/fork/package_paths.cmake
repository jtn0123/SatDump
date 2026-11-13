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

# Two upstream examples also contain literal home paths (not filename macros).
# Build neutral package copies, leaving test/source files and their line numbers intact.
function(satdump_package_examples source_list relative_source)
    set(_original "${CMAKE_SOURCE_DIR}/${relative_source}")
    set(_copy "${CMAKE_BINARY_DIR}/package-sources/${relative_source}")
    file(READ "${_original}" _text)
    string(REGEX REPLACE "/home/[^/\"]+/" "/data/" _text "${_text}")
    get_filename_component(_copy_dir "${_copy}" DIRECTORY)
    file(MAKE_DIRECTORY "${_copy_dir}")
    file(WRITE "${_copy}" "${_text}")
    get_filename_component(_original_dir "${_original}" DIRECTORY)
    set_source_files_properties("${_copy}" PROPERTIES
        INCLUDE_DIRECTORIES "${_original_dir}"
        COMPILE_OPTIONS "-ffile-prefix-map=${_copy}=${SATDUMP_PACKAGE_SOURCE_ROOT}/${relative_source}")
    set(_sources "${${source_list}}")
    list(REMOVE_ITEM _sources "${_original}")
    list(APPEND _sources "${_copy}")
    set(${source_list} "${_sources}" PARENT_SCOPE)
endfunction()
