# Fork: helper for tests/package/deb_settings.sh, run with cmake -P.
#
#   cmake -DCPACK_CONFIG=<build>/CPackConfig.cmake -P query.cmake
#       prints the Debian package settings CPack will use, one NAME=value line each
#   cmake -DCPACK_CONFIG=<build>/CPackConfig.cmake -DGENERATOR=DEB -P query.cmake
#       does what cpack does before it makes a package of that type: reads the settings, sets
#       CPACK_GENERATOR and reads the project's CPACK_PROJECT_CONFIG_FILE, which may stop it
#       (exit code 1). Run with -P, find_program searches only PATH.
#   cmake -DSOURCE=<source dir> -DVERSION=2.0.0 -DTYPE=alpha -DDATE=20261111.230102 -DHASH=44f17297f -P query.cmake
#       prints the Debian version cmake/fork/debian_version.cmake makes from those values

if(DEFINED CPACK_CONFIG AND DEFINED GENERATOR)
    include("${CPACK_CONFIG}")
    set(CPACK_GENERATOR "${GENERATOR}")
    if(CPACK_PROJECT_CONFIG_FILE)
        include("${CPACK_PROJECT_CONFIG_FILE}")
    endif()
    message(NOTICE "${GENERATOR}: the CPack project configuration let it through")
elseif(DEFINED CPACK_CONFIG)
    include("${CPACK_CONFIG}")
    foreach(_name IN ITEMS
            CPACK_GENERATOR
            CPACK_PACKAGING_INSTALL_PREFIX
            CPACK_PACKAGE_NAME
            CPACK_PACKAGE_VERSION
            CPACK_DEBIAN_PACKAGE_NAME
            CPACK_DEBIAN_PACKAGE_VERSION
            CPACK_DEBIAN_PACKAGE_RELEASE
            CPACK_DEBIAN_PACKAGE_SHLIBDEPS
            CPACK_DEBIAN_PACKAGE_SHLIBDEPS_PRIVATE_DIRS
            CPACK_DEBIAN_DEBUGINFO_PACKAGE
            CPACK_DEBIAN_PACKAGE_DEPENDS
            CPACK_DEBIAN_PACKAGE_RECOMMENDS
            CPACK_DEBIAN_PACKAGE_SUGGESTS
            CPACK_DEBIAN_PACKAGE_PROVIDES
            CPACK_DEBIAN_PACKAGE_CONFLICTS
            CPACK_DEBIAN_PACKAGE_REPLACES
            CPACK_DEBIAN_FILE_NAME
            CPACK_PROJECT_CONFIG_FILE)
        message(NOTICE "${_name}=${${_name}}")
    endforeach()
else()
    include("${SOURCE}/cmake/fork/debian_version.cmake")
    satdump_debian_version(_v "${VERSION}" "${TYPE}" "${DATE}" "${HASH}")
    message(NOTICE "${_v}")
endif()
