# Fork: Linux builds use the system's muparser (Debian's libmuparser-dev) instead of compiling
# the copy in src-core/libs/muparser. Windows, macOS and Android keep compiling that copy, which
# stays in the tree unchanged. Code includes muparser through src-core/utils/muparser.h, which
# makes the same choice (keep the two conditions alike).
# Included by src-core/CMakeLists.txt right after the satdump_core target is made.

if(CMAKE_SYSTEM_NAME STREQUAL "Linux") # Android builds have their own system name
    if(NOT PKG_CONFIG_FOUND)
        message(FATAL_ERROR "pkg-config is needed to find muparser: install Debian's pkgconf")
    endif()
    pkg_check_modules(MUPARSER IMPORTED_TARGET muparser>=2.3.3)
    if(NOT MUPARSER_FOUND)
        message(FATAL_ERROR "muparser 2.3.3 or newer not found: install Debian's libmuparser-dev")
    endif()

    # Leave the bundled copy out (libs/muparserx is another library and stays)
    get_target_property(_satdump_core_sources satdump_core SOURCES)
    list(FILTER _satdump_core_sources EXCLUDE REGEX "/src-core/libs/muparser/")
    set_property(TARGET satdump_core PROPERTY SOURCES ${_satdump_core_sources})
    unset(_satdump_core_sources)

    # Public: plugins (bitview) call muparser themselves
    target_link_libraries(satdump_core PUBLIC PkgConfig::MUPARSER)
endif()
