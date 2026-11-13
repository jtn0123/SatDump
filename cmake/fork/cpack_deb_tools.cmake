# Fork: the tools the satdump-fork Debian package needs, checked before CPack makes it.
#
# CPack includes this file (CPACK_PROJECT_CONFIG_FILE, set in the CPack section of the
# top-level CMakeLists.txt) once for each package type, before it installs anything. The Debian
# package has its Depends written by dpkg-shlibdeps (CPACK_DEBIAN_PACKAGE_SHLIBDEPS) and its
# debug information split off into a separate debug-symbols package
# (CPACK_DEBIAN_DEBUGINFO_PACKAGE), which need programs CPack does not insist on in the same way:
#   - dpkg-shlibdeps (package dpkg-dev). Without it CPack only printed a note and made a
#     package with an empty Depends, which installs but cannot run.
#   - file (package file). Without it CPack stopped only after the whole install, with a
#     message that did not say which package to install.
#   - objcopy and readelf (package binutils), which split off the debug information and read
#     each file's build-id. Without them CPack also stopped only after the whole install.
# Now each is looked for first, and making a Debian package stops at once, naming the Debian
# packages to install. Other package types are not affected. A file that is there but cannot
# be run counts as missing (policy CMP0109; CPack leaves it unset, which accepts any readable
# file and only warns).

if(CPACK_GENERATOR STREQUAL "DEB" AND (CPACK_DEBIAN_PACKAGE_SHLIBDEPS OR CPACK_DEBIAN_DEBUGINFO_PACKAGE))
    cmake_policy(PUSH)
    if(POLICY CMP0109)
        cmake_policy(SET CMP0109 NEW)
    endif()
    set(_satdump_deb_missing "")
    if(CPACK_DEBIAN_PACKAGE_SHLIBDEPS)
        find_program(SATDUMP_DEB_SHLIBDEPS_PROGRAM dpkg-shlibdeps)
        if(NOT SATDUMP_DEB_SHLIBDEPS_PROGRAM)
            list(APPEND _satdump_deb_missing "dpkg-shlibdeps (package dpkg-dev)")
        endif()
    endif()
    find_program(SATDUMP_DEB_FILE_PROGRAM file)
    if(NOT SATDUMP_DEB_FILE_PROGRAM)
        list(APPEND _satdump_deb_missing "file (package file)")
    endif()
    if(CPACK_DEBIAN_DEBUGINFO_PACKAGE)
        find_program(SATDUMP_DEB_OBJCOPY_PROGRAM objcopy)
        find_program(SATDUMP_DEB_READELF_PROGRAM readelf)
        if(NOT SATDUMP_DEB_OBJCOPY_PROGRAM)
            list(APPEND _satdump_deb_missing "objcopy (package binutils)")
        endif()
        if(NOT SATDUMP_DEB_READELF_PROGRAM)
            list(APPEND _satdump_deb_missing "readelf (package binutils)")
        endif()
    endif()
    cmake_policy(POP)
    if(_satdump_deb_missing)
        list(JOIN _satdump_deb_missing " and " _satdump_deb_missing)
        message(FATAL_ERROR
            "Not making the ${CPACK_DEBIAN_PACKAGE_NAME} Debian package: "
            "${_satdump_deb_missing} not found or not executable (needed to write its "
            "Depends and to split off its debug symbols). "
            "Install with: sudo apt install dpkg-dev file binutils")
    endif()
endif()
