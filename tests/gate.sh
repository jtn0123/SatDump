#!/bin/bash
# The one check command: build, warning ratchet, install into the build folder, run all tests.
#
#   tests/gate.sh [build-dir] [--update]
#
# The build folder (default: build) must be configured with -DSATDUMP_TESTS=ON, normally
# with the fork preset:
#   cmake -C cmake/fork/pi.cmake -DSATDUMP_TESTS=ON -S . -B build -G Ninja
# --update lowers the warning baseline after warnings were fixed.
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
FAIL=0
start=$(date +%s)

# Clean first so every warning is seen; ccache makes the rebuild quick
echo "gate: building $B"
if ! cmake --build "$B" --clean-first -j"$JOBS" > "$B/gate-build.log" 2>&1; then
    grep -E "error:|FAILED|Error [0-9]" "$B/gate-build.log" | head -30
    echo "gate: build FAILED (log: $B/gate-build.log)"
    exit 1
fi

python3 -I -B "$ROOT/tests/warnings/ratchet.py" "$B/gate-build.log" "$ROOT" \
    "$ROOT/tests/warnings/$(uname -m).txt" "${UPDATE[@]}" || FAIL=1

# The cli tests run the installed program, never the build tree or the source folder.
# Each run installs over the last one (files are replaced, nothing is deleted).
if ! cmake --install "$B" --prefix "$B/check-prefix" > "$B/gate-install.log" 2>&1; then
    tail -5 "$B/gate-install.log"
    echo "gate: install FAILED"
    exit 1
fi

ctest --test-dir "$B" --output-on-failure -j4 || FAIL=1

echo "gate: $([ $FAIL = 0 ] && echo PASSED || echo FAILED) in $(($(date +%s) - start)) s"
exit $FAIL
