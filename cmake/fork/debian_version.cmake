# Fork: the version of the satdump-fork Debian package (the CPack section of the top-level
# CMakeLists.txt uses it; tests/package/deb_settings.sh tests it).
#
# SatDump's own version string, for example 2.0.0-alpha-1aedd60, does not work as a Debian
# version: Debian reads everything after the last hyphen as the package revision, so two
# builds were ordered by their commit hashes, which say nothing about which one is newer.
# This makes
#
#     <version>~<type>+git<commit date>.<commit time>.<hash>
#     for example 2.0.0~alpha+git20261111.230102.44f17297f
#
# "~" puts a pre-release (alpha, beta) before the release itself (2.0.0), and the commit date
# and time (UTC) put a later commit after an earlier one. Without git information (a source
# tree that is not a git clone) it is just <version>~<type>, which sorts before every build
# made from a clone. The Debian revision ("-1") is added by CPack.
#
#   satdump_debian_version(<out var> <version> <type> <commit date YYYYMMDD.HHMMSS> <hash>)

function(satdump_debian_version out version type date hash)
    # Debian allows letters, digits and . + ~ in a version; anything else becomes a dot
    string(REGEX REPLACE "[^A-Za-z0-9.+~]" "." version "${version}")
    string(REGEX REPLACE "[^A-Za-z0-9.]" "." type "${type}")
    string(REGEX REPLACE "[^A-Za-z0-9]" "" hash "${hash}")

    set(_debian_version "${version}")
    if(NOT type STREQUAL "")
        string(APPEND _debian_version "~${type}")
    endif()
    if(date MATCHES "^[0-9]+\\.[0-9]+$" AND NOT hash STREQUAL "")
        string(APPEND _debian_version "+git${date}.${hash}")
    endif()
    set(${out} "${_debian_version}" PARENT_SCOPE)
endfunction()
