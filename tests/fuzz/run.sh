#!/bin/bash
# Run one fuzz harness.
#   run.sh <harness> 0          a short, repeatable run (fixed seed, 20000 inputs): for ctest
#   run.sh <harness> <seconds>  fuzz for that long; new interesting inputs are kept in
#                               tests/fuzz/corpus/<name>/ (not committed) for the next run
# A crash, memory error or undefined behaviour stops the run, prints the report and keeps
# the input that caused it as ./crash-<hash>, or as ./timeout-* or ./oom-* for a hang or a
# runaway allocation.
set -uo pipefail
BIN=${1:?usage: $0 <harness> <seconds, or 0 for the short repeatable run>}
SECS=${2:-0}
NAME=$(basename "$BIN")
LIMITS=(-max_len=16384 -rss_limit_mb=2048 -timeout=20 -print_final_stats=1)
DICT="$(cd "$(dirname "$0")" && pwd)/${NAME#fuzz_}.dict"
[ -f "$DICT" ] && LIMITS+=(-dict="$DICT")
export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_odr_violation=0:detect_leaks=0}
export UBSAN_OPTIONS=${UBSAN_OPTIONS:-print_stacktrace=1:halt_on_error=1}
if [ "$SECS" = 0 ]; then
    CORPUS=$(mktemp -d)
    trap 'rm -rf "$CORPUS"' EXIT
    "$BIN" -seed=1 -runs=20000 "${LIMITS[@]}" "$CORPUS"
    exit  # with the fuzzer's status; the trap removes the temporary corpus
fi
CORPUS="$(cd "$(dirname "$0")" && pwd)/corpus/${NAME#fuzz_}"
mkdir -p "$CORPUS"
exec "$BIN" -max_total_time="$SECS" "${LIMITS[@]}" "$CORPUS"
