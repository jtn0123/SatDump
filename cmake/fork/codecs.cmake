# Fork: the compression libraries SatDump decodes radio data and pictures with. Included by
# src-core/CMakeLists.txt right after satdump_core is created.
#
# On Linux (not Android), Debian's copies are used, so Debian's security fixes reach SatDump:
#   bzip2     libbz2-dev        MetOp admin messages, Himawari, first-party files
#   OpenJPEG  libopenjp2-7-dev  J2K pictures (GOES GRB, FY-4, GK-2A, ORB, .j2k files)
#   libaec    libaec-dev        CCSDS 121 Rice (JPSS VIIRS and OMPS, GOES LRIT, Landsat)
# A missing one stops the configure step with the package to install; there is no fall back
# to the bundled copy. On Windows, macOS and Android the bundled copies in src-core/libs are
# built into satdump_core as before, and their folders are put on the include path, so the
# code includes <bzlib.h>, <openjpeg.h>, <libaec.h> and <szlib.h> everywhere.
#
# A target that includes <libaec.h> or <szlib.h> calls satdump_link_aec(<target>) (below); the
# bundled libaec folder is on the include path of those targets only.
#
# muparserx (src-core/libs/muparserx) is not compiled on any platform: nothing calls it. Its
# files, like the bundled codecs, stay in the tree so upstream merges stay simple.

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT ANDROID)
    set(SATDUMP_SYSTEM_CODECS ON)
else()
    set(SATDUMP_SYSTEM_CODECS OFF)
endif()

# Fork: the core's source list came from a glob that also took the unused muparserx (39 files)
# and, on Linux, the bundled codecs; they are taken out of satdump_core here
get_target_property(_satdump_core_sources satdump_core SOURCES)
list(FILTER _satdump_core_sources EXCLUDE REGEX "/src-core/libs/muparserx/")
if(SATDUMP_SYSTEM_CODECS)
    list(FILTER _satdump_core_sources EXCLUDE REGEX "/src-core/libs/(bzlib|openjp2|aec)/")
endif()
set_property(TARGET satdump_core PROPERTY SOURCES ${_satdump_core_sources})
unset(_satdump_core_sources)

add_library(satdump_aec INTERFACE)

if(SATDUMP_SYSTEM_CODECS)
    message("-- Using the system bzip2, OpenJPEG and libaec")

    # bzip2: the core's header libs/bzlib_utils.h uses it, so it is part of the core's interface
    find_package(BZip2)
    if(NOT BZIP2_FOUND)
        message(FATAL_ERROR "bzip2 was not found. On Debian or Ubuntu: apt install libbz2-dev")
    endif()
    target_link_libraries(satdump_core PUBLIC BZip2::BZip2)

    # OpenJPEG (the core's J2K loaders; the tests and fuzzers include <openjpeg.h> too)
    if(NOT PKG_CONFIG_FOUND)
        message(FATAL_ERROR "pkg-config was not found (needed to find OpenJPEG). On Debian or Ubuntu: apt install pkgconf")
    endif()
    pkg_check_modules(OPENJP2 IMPORTED_TARGET libopenjp2)
    if(NOT OPENJP2_FOUND)
        message(FATAL_ERROR "OpenJPEG (libopenjp2) was not found. On Debian or Ubuntu: apt install libopenjp2-7-dev")
    endif()
    target_link_libraries(satdump_core PUBLIC PkgConfig::OPENJP2)

    # libaec: Debian's libaec-dev has no pkg-config file. The SZ_* functions are in libsz,
    # which comes from the same source package.
    find_path(SATDUMP_AEC_INCLUDE_DIR NAMES libaec.h szlib.h)
    find_library(SATDUMP_AEC_LIBRARY NAMES aec)
    if(SATDUMP_AEC_LIBRARY)
        get_filename_component(_satdump_aec_dir ${SATDUMP_AEC_LIBRARY} DIRECTORY)
        find_library(SATDUMP_SZ_LIBRARY NAMES sz HINTS ${_satdump_aec_dir} NO_DEFAULT_PATH)
        unset(_satdump_aec_dir)
    endif()
    if(NOT SATDUMP_AEC_INCLUDE_DIR OR NOT EXISTS ${SATDUMP_AEC_INCLUDE_DIR}/szlib.h OR NOT SATDUMP_AEC_LIBRARY OR NOT SATDUMP_SZ_LIBRARY)
        message(FATAL_ERROR "libaec (libaec.h, szlib.h, libaec and libsz) was not found. On Debian or Ubuntu: apt install libaec-dev")
    endif()
    target_include_directories(satdump_aec INTERFACE ${SATDUMP_AEC_INCLUDE_DIR})
    target_link_libraries(satdump_aec INTERFACE ${SATDUMP_AEC_LIBRARY} ${SATDUMP_SZ_LIBRARY})
else()
    # BEFORE: the folders added for every target by the top-level CMakeLists.txt (on macOS
    # /opt/homebrew/include, where Homebrew's libaec puts its libaec.h and szlib.h) would
    # otherwise be searched first, and their headers used with the bundled code
    target_include_directories(satdump_core BEFORE PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/libs/bzlib
        ${CMAKE_CURRENT_SOURCE_DIR}/libs/openjp2)
    target_include_directories(satdump_aec INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/libs/aec)
    set_property(GLOBAL PROPERTY SATDUMP_BUNDLED_AEC_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/libs/aec)
endif()

# Fork: a target that includes <libaec.h> or <szlib.h> calls this. On Linux it links the system
# libaec and libsz. Elsewhere the code is in satdump_core, and the bundled headers are put in
# front of the target's own include folders: folders from linked targets come after those, so
# on macOS Homebrew's libaec.h and szlib.h (in /opt/homebrew/include) would be used otherwise.
function(satdump_link_aec target)
    target_link_libraries(${target} PUBLIC satdump_aec)
    get_property(_bundled_aec GLOBAL PROPERTY SATDUMP_BUNDLED_AEC_INCLUDE_DIR)
    if(_bundled_aec)
        target_include_directories(${target} BEFORE PRIVATE ${_bundled_aec})
    endif()
endfunction()

# Fork: the headers of code satdump_core does not build are left out of its installed headers
# (src-core/CMakeLists.txt adds this to its install rules): muparserx everywhere, and on Linux
# the bundled codecs, whose functions the installed library does not have. A plugin built
# against the installed headers then uses the system's headers, as SatDump itself does.
set(SATDUMP_CORE_HEADERS_EXCLUDE REGEX "/libs/muparserx$" EXCLUDE)
if(SATDUMP_SYSTEM_CODECS)
    list(APPEND SATDUMP_CORE_HEADERS_EXCLUDE REGEX "/libs/(bzlib|openjp2|aec)$" EXCLUDE)
endif()
