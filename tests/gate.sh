#!/bin/bash
# The one check command: build, warning ratchet, install into the build folder, run all tests.
#
#   tests/gate.sh [build-dir] [--update]
#
# The build folder (default: build) must be configured with -DSATDUMP_TESTS=ON, normally
# with the fork preset:
#   cmake -C cmake/fork/pi.cmake -DSATDUMP_TESTS=ON -S . -B build -G Ninja
# A second folder configured with -DSATDUMP_SANITIZERS=address,undefined runs the same tests
# with memory and undefined-behaviour checks.
# --update lowers the warning baseline after warnings were fixed.
# On a receiving station it never starts during a pass or when the disk is low (tests/guard.sh).
# Exit code: 0 all passed, 1 something failed.
set -uo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
B=${1:-build}
UPDATE=()
[ "${2:-}" = "--update" ] && UPDATE=(--update)
[ "${1:-}" = "--update" ] && { B=build; UPDATE=(--update); }

grep -q '^SATDUMP_TESTS:BOOL=ON' "$B/CMakeCache.txt" 2>/dev/null ||
    { echo "gate: $B is not configured with -DSATDUMP_TESTS=ON"; exit 1; }
JOBS=$(nproc 2>/dev/null || sysctl -n hw.ncpu)
. "$ROOT/tests/guard.sh"
FAIL=0
start=$(date +%s)

# Clean first so every warning is seen; ccache makes the rebuild quick
echo "gate: building $B"
if ! cmake --build "$B" --clean-first -j"$JOBS" > "$B/gate-build.log" 2>&1; then
    grep -E "error:|FAILED|Error [0-9]" "$B/gate-build.log" | head -30
    echo "gate: build FAILED (log: $B/gate-build.log)"
    exit 1
fi

# Warnings are checked on normal builds only (sanitizer flags change what the compiler reports)
if grep -q '^SATDUMP_SANITIZERS:STRING=.' "$B/CMakeCache.txt"; then
    echo "ratchet: skipped for a sanitizer build"
else
    python3 -I -B "$ROOT/tests/warnings/ratchet.py" "$B/gate-build.log" "$ROOT" \
        "$ROOT/tests/warnings/$(uname -m).txt" "${UPDATE[@]}" || FAIL=1
fi

# The cli tests run the installed program (in SATDUMP_CHECK_PREFIX), never the build tree.
# A prefix inside the build folder (the default, <build>/check-prefix) is emptied first, so it
# holds this build only. Installing over the last run kept the files an earlier install had and
# this one has not, such as libraries from an install with another libdir, and the cli tests
# ran those (cli_install_fresh checks for them). A prefix elsewhere is installed over.
BUILD=$(cd "$B" && pwd)
PREFIX=$(sed -n 's/^SATDUMP_CHECK_PREFIX:PATH=//p' "$B/CMakeCache.txt")
PREFIX=${PREFIX:-$BUILD/check-prefix}
case "$PREFIX" in
    *..*) ;;
    "$BUILD"/?*) rm -rf "$PREFIX" ;;
esac
if ! cmake --install "$B" --prefix "$PREFIX" > "$B/gate-install.log" 2>&1; then
    tail -5 "$B/gate-install.log"
    echo "gate: install FAILED"
    exit 1
fi

ctest --test-dir "$B" --output-on-failure -j4 || FAIL=1

echo "gate: $([ $FAIL = 0 ] && echo PASSED || echo FAILED) in $(($(date +%s) - start)) s"
exit $FAIL
