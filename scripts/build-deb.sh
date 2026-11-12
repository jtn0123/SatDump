#!/bin/bash
# Fork: builds the satdump-fork Debian package and prints the path of the .deb.
#
#   scripts/build-deb.sh [build folder]        (default: build-deb in the source tree)
#
# Run it on arm64 Debian 13 (trixie): the Raspberry Pi 5 itself, or any other arm64 Debian 13
# machine such as a VM, which is much faster. It builds natively, with the Pi preset
# (cmake/fork/pi.cmake: every decoder, tuned for the Pi 5's Cortex-A76 cores, relocatable) and
# the install prefix /usr, then makes the package with CPack. CPack runs dpkg-shlibdeps, which
# writes the package's Depends from what the programs, libraries and plugins link against, so
# they name the Debian 13 packages of this machine. The version sorts by commit date, so build
# from a git clone (see cmake/fork/debian_version.cmake).
#
# Needs: every build package in packages.builder (dpkg-dev, which has dpkg-shlibdeps, and file
# among them), and ninja-build. It stops before building when one of them is missing, as a
# radio whose -dev package is missing would quietly get no plugin; SATDUMP_DEB_ALLOW_MISSING=1
# builds anyway, and the radios left out are listed at the end. ccache is used when it is
# installed. The build folder takes about 5 GB.
# On a receiving station it never starts during a pass, or with less than 9 GB free (the 5 GB
# for the build plus the 4 GB tests/guard.sh keeps for a pass's recording; SATDUMP_MIN_FREE_GB
# changes it), and it runs at the lowest priority (tests/guard.sh; SATDUMP_GUARD=off skips the
# checks on machines that never receive passes).
#
# To check the package without installing it: tests/package/check_deb.sh (see its header).
# To install it on the Pi (apt removes the upstream satdump package, which this one replaces):
#   sudo apt install ./satdump-fork_<version>_arm64.deb
set -uo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
B=${1:-$ROOT/build-deb}

fail() {
    echo "build-deb: $*" >&2
    exit 1
}
command -v dpkg-shlibdeps > /dev/null ||
    fail "dpkg-shlibdeps not found; install it with: sudo apt install dpkg-dev"
command -v file > /dev/null ||
    fail "file not found (CPack needs it for dpkg-shlibdeps); install it with: sudo apt install file"
arch=$(dpkg --print-architecture)
[ "$arch" = arm64 ] ||
    fail "this is the Raspberry Pi 5 package (Cortex-A76) and builds natively on arm64 only; this machine is $arch"
. /etc/os-release
[ "${ID:-}" = debian ] && [ "${VERSION_ID:-}" = 13 ] ||
    echo "build-deb: warning: this is ${PRETTY_NAME:-not Debian 13}; the package will depend on this system's library packages, not Debian 13's" >&2

# Every build package must be installed: a radio library whose -dev package is missing is left
# out of the build with only a configure message, so the package would lack that radio's plugin
missing=()
while IFS= read -r pkg; do
    pkg=${pkg%%#*}
    pkg=${pkg//[[:space:]]/}
    [ -n "$pkg" ] || continue
    [ "$(dpkg-query -W -f='${db:Status-Status}' "$pkg" 2> /dev/null)" = installed ] || missing+=("$pkg")
done < "$ROOT/packages.builder"
if [ ${#missing[@]} -gt 0 ]; then
    if [ "${SATDUMP_DEB_ALLOW_MISSING:-}" = 1 ]; then
        echo "build-deb: warning: building without ${missing[*]} (SATDUMP_DEB_ALLOW_MISSING=1)" >&2
    else
        fail "these build packages (packages.builder) are not installed: ${missing[*]}
Install them with: sudo apt install ${missing[*]}
or set SATDUMP_DEB_ALLOW_MISSING=1 to build a package without the radios they are for"
    fi
fi

mkdir -p "$B"
B=$(cd "$B" && pwd)
GUARD_NAME=build-deb
SATDUMP_MIN_FREE_GB=${SATDUMP_MIN_FREE_GB:-9}
. "$ROOT/tests/guard.sh"

options=(-C "$ROOT/cmake/fork/pi.cmake" -S "$ROOT" -B "$B" -DCMAKE_INSTALL_PREFIX=/usr -DSATDUMP_TESTS=OFF)
if [ ! -f "$B/CMakeCache.txt" ] && command -v ninja > /dev/null; then
    options+=(-G Ninja)
fi
if command -v ccache > /dev/null; then
    options+=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
fi

# Configuring every time keeps the version (commit date and hash) up to date
echo "build-deb: configuring $B"
cmake "${options[@]}" > "$B/build-deb-configure.log" 2>&1 ||
    { tail -20 "$B/build-deb-configure.log"; fail "configure failed (log: $B/build-deb-configure.log)"; }
grep -E "^-- Debian package:|No git information" "$B/build-deb-configure.log" || true

echo "build-deb: building (log: $B/build-deb-build.log)"
cmake --build "$B" -j"$(nproc)" > "$B/build-deb-build.log" 2>&1 ||
    { grep -E "error:|FAILED" "$B/build-deb-build.log" | head -20; fail "build failed (log: $B/build-deb-build.log)"; }

echo "build-deb: packaging (log: $B/build-deb-cpack.log)"
rm -f "$B"/satdump-fork_*.deb
(cd "$B" && cpack -G DEB) > "$B/build-deb-cpack.log" 2>&1 ||
    { tail -20 "$B/build-deb-cpack.log"; fail "packaging failed (log: $B/build-deb-cpack.log)"; }

debs=("$B"/satdump-fork_*.deb)
[ ${#debs[@]} = 1 ] && [ -f "${debs[0]}" ] || fail "expected one satdump-fork_*.deb in $B, found: ${debs[*]}"
# The plugins configure left out because their library was not found (radios, audio)
skipped=$(sed -n 's/ Library could not be found! Not building\.$//p' "$B/build-deb-configure.log" | paste -sd, | sed 's/,/, /g')
[ -z "$skipped" ] || echo "build-deb: no plugin in this package for: $skipped"
echo "build-deb: $(du -h "${debs[0]}" | cut -f1) package; check it with tests/package/check_deb.sh"
echo "${debs[0]}"
