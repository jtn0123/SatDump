#!/bin/bash
# Fork: inject an FFT thread creation failure into the real record setup and cleanup.
TESTS=$(cd "$(dirname "$0")/.." && pwd)
. "$TESTS/cli/common.sh"
FAULT=${2:?usage: $0 <install prefix> <record FFT failure executable> <probe plugin>}
PLUGIN=${3:?usage: $0 <install prefix> <record FFT failure executable> <probe plugin>}
mkdir plugins
cp "$PLUGIN" plugins/ || exit 1
SATDUMP_TEST_PREFIX="$PREFIX" timeout -k 5 25 "$FAULT" record "$WORK/failed" --source fork_probe --samplerate 6000000 \
    --frequency 1701300000 --probe_stream --baseband_format cf32 --decimation 2 --fft_enable > "$WORK/fault.log" 2>&1
got=$?
check "fft-start-failure (exit $got, expected 2)" test "$got" = 2
check injected grep -q 'Injected FFT thread-start failure' "$WORK/fault.log"
check clean bash -c '! grep -E "BLOCK SHOULD BE STOPPED|pure virtual|stopped on an error that was not handled" "$1"' _ "$WORK/fault.log"
[ "$FAILED" = 0 ] || cat "$WORK/fault.log"
exit $FAILED
