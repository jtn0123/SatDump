#!/bin/bash
# Fork: the Debian package settings a build folder is configured with (its CPackConfig.cmake),
# checked without building a package: 1. the version scheme, 2. the settings, 3. that making
# the package stops at once when dpkg-shlibdeps, file, objcopy or readelf is missing.
# tests/package/check_deb.sh checks a built package.
#
#   tests/package/deb_settings.sh <build dir> <source dir> <install prefix> [cmake]
#
# <install prefix> is where tests/gate.sh installed the build (SATDUMP_CHECK_PREFIX); the
# folders dpkg-shlibdeps searches for SatDump's own libraries must hold them there too.
# [cmake] is the cmake program that configured the build (default: the one on PATH).
# Needs dpkg (in every Debian install) for its version checks.
set -u
usage="usage: $0 <build dir> <source dir> <install prefix> [cmake]"
B=${1:?$usage}
SRC=${2:?$usage}
PREFIX=${3:?$usage}
CMAKE=${4:-$(command -v cmake)}
FAILED=0
ok() { echo "ok   $*"; }
fail() { echo "FAIL $*"; FAILED=1; }

# The Debian version cmake/fork/debian_version.cmake makes from <version> <type> <date> <hash>
debver() {
    "$CMAKE" -DSOURCE="$SRC" -DVERSION="$1" -DTYPE="$2" -DDATE="$3" -DHASH="$4" \
        -P "$SRC/tests/package/query.cmake" 2>&1
}

# 1. Versions: valid for dpkg, and ordered by release stage, then by commit date (never by hash)
fixtures=(
    "2.0.0 alpha - -"                                  # a source tree without git information
    "2.0.0 alpha 20261111.230102 44f17297f"
    "2.0.0 alpha 20261111.230103 0a1b2c3d4"            # one second later, smaller hash
    "2.0.0 alpha 20261205.080000 00000001a"
    "2.0.0 beta 20260101.000000 fffffffff"             # beta after every alpha
    "2.0.0 - 20260101.000000 123456789"                # the release after every pre-release
    "2.0.1 alpha 20250101.000000 000000000"            # the next version after that
)
prev=""
for f in "${fixtures[@]}"; do
    read -r v t d h <<< "$f"
    [ "$t" = - ] && t=""
    [ "$d" = - ] && d=""
    [ "$h" = - ] && h=""
    cur="$(debver "$v" "$t" "$d" "$h")-1"
    if ! dpkg --validate-version "$cur" 2> /dev/null; then
        fail "version $cur (from $f) is not a valid Debian version"
    elif [ -n "$prev" ] && ! dpkg --compare-versions "$prev" lt "$cur"; then
        fail "version $prev does not sort before $cur"
    else
        ok "version $cur"
    fi
    prev=$cur
done

# 2. The settings CPack was configured with
[ -f "$B/CPackConfig.cmake" ] || { echo "FAIL no $B/CPackConfig.cmake"; exit 1; }
declare -A cfg
while IFS='=' read -r name value; do
    cfg[$name]=$value
done < <("$CMAKE" -DCPACK_CONFIG="$B/CPackConfig.cmake" -P "$SRC/tests/package/query.cmake" 2>&1)

# want <name> <expected value>
want() {
    if [ "${cfg[$1]-}" = "$2" ]; then
        ok "$1 is $2"
    else
        fail "$1 is '${cfg[$1]-}', expected '$2'"
    fi
}
# What CPack falls back to when the Debian name or version is not set
[ -n "${cfg[CPACK_DEBIAN_PACKAGE_NAME]-}" ] ||
    cfg[CPACK_DEBIAN_PACKAGE_NAME]=$(tr 'A-Z' 'a-z' <<< "${cfg[CPACK_PACKAGE_NAME]-}")
[ -n "${cfg[CPACK_DEBIAN_PACKAGE_VERSION]-}" ] || cfg[CPACK_DEBIAN_PACKAGE_VERSION]=${cfg[CPACK_PACKAGE_VERSION]-}

want CPACK_GENERATOR DEB
want CPACK_PACKAGING_INSTALL_PREFIX /usr
want CPACK_DEBIAN_PACKAGE_NAME satdump-fork
want CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON
# Stripped programs, libraries and plugins, with their debug information in satdump-fork-dbgsym
want CPACK_DEBIAN_DEBUGINFO_PACKAGE ON
want CPACK_DEBIAN_PACKAGE_RELEASE 1
want CPACK_DEBIAN_FILE_NAME DEB-DEFAULT

# It takes the place of the upstream "satdump" package
for field in PROVIDES CONFLICTS REPLACES; do
    if tr ',' '\n' <<< "${cfg[CPACK_DEBIAN_PACKAGE_$field]-}" | grep -qxE '[[:space:]]*satdump[[:space:]]*'; then
        ok "$field satdump"
    else
        fail "$field does not name satdump: '${cfg[CPACK_DEBIAN_PACKAGE_$field]-}'"
    fi
done

# dpkg-shlibdeps writes the library dependencies; nothing hand-written names a shared library
# or a -dev package
depends=${cfg[CPACK_DEBIAN_PACKAGE_DEPENDS]-}
hand=$(tr ',|' '\n\n' <<< "$depends" | sed 's/([^)]*)//; s/^[[:space:]]*//; s/[[:space:]]*$//' |
    grep -E '^lib|-dev$' || true)
if [ -n "$hand" ]; then
    fail "hand-written Depends names libraries or -dev packages: $(echo $hand)"
else
    ok "no hand-written library or -dev dependencies (Depends: '${depends}')"
fi

# The folders dpkg-shlibdeps searches are where libsatdump_core and the plugins are installed
# (paths in the package, relative to its root)
libdir=$(sed -n 's/^CMAKE_INSTALL_LIBDIR:PATH=//p' "$B/CMakeCache.txt")
root=${cfg[CPACK_PACKAGING_INSTALL_PREFIX]#/}
dirs=";${cfg[CPACK_DEBIAN_PACKAGE_SHLIBDEPS_PRIVATE_DIRS]-};"
for d in "$root/$libdir" "$root/$libdir/satdump/plugins"; do
    if [[ $dirs == *";$d;"* ]]; then
        ok "dpkg-shlibdeps searches $d"
    else
        fail "dpkg-shlibdeps does not search $d (it searches '${cfg[CPACK_DEBIAN_PACKAGE_SHLIBDEPS_PRIVATE_DIRS]-}')"
    fi
done
check_installed() {
    if compgen -G "$1" > /dev/null; then ok "installed: ${1#"$PREFIX"/}"; else fail "nothing installed at ${1#"$PREFIX"/}"; fi
}
check_installed "$PREFIX/$libdir/libsatdump_core.so"
check_installed "$PREFIX/$libdir/satdump/plugins/*.so"

# The version of this build: valid, no hyphen of its own, ordered as in part 1
version=${cfg[CPACK_DEBIAN_PACKAGE_VERSION]-}
full=$version${cfg[CPACK_DEBIAN_PACKAGE_RELEASE]:+-${cfg[CPACK_DEBIAN_PACKAGE_RELEASE]}}
if [[ $version == *-* ]] || ! dpkg --validate-version "$full" 2> /dev/null; then
    fail "this build's version '$full' is not a valid Debian version without a hyphen of its own"
elif ! [[ $version =~ ^[0-9][0-9.]*(~[A-Za-z0-9.]+)?(\+git[0-9]{8}\.[0-9]{6}\.[0-9a-f]+)?$ ]]; then
    fail "this build's version '$full' is not <version>[~<type>][+git<date>.<time>.<hash>]"
else
    ok "this build's version is $full"
fi

# The Dockerfile copies the package file by its name
name=${cfg[CPACK_DEBIAN_PACKAGE_NAME]-}
if [ -n "$name" ] && grep -qF "/${name}_*.deb" "$SRC/Dockerfile"; then
    ok "the Dockerfile copies ${name}_*.deb"
else
    fail "the Dockerfile does not copy ${name}_*.deb"
fi

# 3. Making the package stops at once, naming the Debian packages to install, when a program
# that dpkg-shlibdeps or the debug-symbols split needs is missing
# (cmake/fork/cpack_deb_tools.cmake). Without the check, CPack made a package with an empty
# Depends (no dpkg-shlibdeps), or stopped after the whole install (no file, objcopy or
# readelf). This runs cpack's first step (query.cmake) with PATH holding only stand-ins for the
# programs named.
# cpack_start <package type> <programs on PATH...>: sets rc and out (CMake wraps its messages;
# out has them on one line). A name ending in :noexec is a file that is there but cannot be run.
cpack_start() {
    local generator=$1 bin p
    shift
    bin=$(mktemp -d)
    for p in "$@"; do
        printf '#!/bin/sh\nexit 0\n' > "$bin/${p%:noexec}"
        [ "$p" != "${p%:noexec}" ] || chmod +x "$bin/$p"
    done
    out=$(env PATH="$bin" "$CMAKE" -DCPACK_CONFIG="$B/CPackConfig.cmake" -DGENERATOR="$generator" \
        -P "$SRC/tests/package/query.cmake" 2>&1)
    rc=$?
    out=$(tr -s '[:space:]' ' ' <<< "$out")
    rm -rf "$bin"
}
cpack_start DEB dpkg-shlibdeps file objcopy readelf
[ $rc = 0 ] && ok "DEB with dpkg-shlibdeps, file, objcopy and readelf: goes ahead" ||
    fail "DEB with dpkg-shlibdeps, file, objcopy and readelf: stopped (exit $rc): $out"
# stops <label> <text the message must hold> <programs on PATH...>
stops() {
    local label=$1 text=$2
    shift 2
    cpack_start DEB "$@"
    if [ $rc != 0 ] && grep -qF -- "$text" <<< "$out" && grep -qF "sudo apt install dpkg-dev file binutils" <<< "$out"; then
        ok "DEB $label: stops at once and names $text"
    else
        fail "DEB $label: did not stop naming $text (exit $rc): $out"
    fi
}
stops "without dpkg-shlibdeps" "dpkg-shlibdeps (package dpkg-dev)" file objcopy readelf
stops "without file" "file (package file)" dpkg-shlibdeps objcopy readelf
stops "with a dpkg-shlibdeps that cannot be run" "dpkg-shlibdeps (package dpkg-dev)" dpkg-shlibdeps:noexec file objcopy readelf
stops "without objcopy" "objcopy (package binutils)" dpkg-shlibdeps file readelf
stops "without readelf" "readelf (package binutils)" dpkg-shlibdeps file objcopy
stops "with an objcopy that cannot be run" "objcopy (package binutils)" dpkg-shlibdeps file objcopy:noexec readelf
stops "with a readelf that cannot be run" "readelf (package binutils)" dpkg-shlibdeps file objcopy readelf:noexec
cpack_start TGZ
[ $rc = 0 ] && ok "TGZ with none of them: goes ahead (other package types are not affected)" ||
    fail "TGZ with none of them: stopped (exit $rc): $out"

exit $FAILED
