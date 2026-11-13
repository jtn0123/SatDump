#!/bin/bash
# Fork: operational live startup failures must clean up and use exit 2, like record.
TESTS=$(cd "$(dirname "$0")/.." && pwd)
. "$TESTS/cli/common.sh"
FAULT=${2:?usage: $0 <install prefix> <live failure executable> <probe plugin>}
PLUGIN=${3:?usage: $0 <install prefix> <live failure executable> <probe plugin>}
mkdir plugins
cp "$PLUGIN" plugins/ || exit 1
printf '{}\n' > "$WORK/vfos.json"
for mode in fft vfo; do
    extra=(--fft_enable)
    [ "$mode" = vfo ] && extra=(--multi_vfo "$WORK/vfos.json")
    SATDUMP_TEST_PREFIX="$PREFIX" timeout -k 5 25 "$FAULT" live metop_ahrpt "$WORK/failed-$mode" \
        --source fork_probe --samplerate 6000000 --frequency 1701300000 --probe_stream "${extra[@]}" > "$WORK/$mode.log" 2>&1
    got=$?
    check "$mode-start-failure (exit $got, expected 2)" test "$got" = 2
    check "$mode-injected" grep -q 'Injected .*thread-start failure' "$WORK/$mode.log"
    check "$mode-command-error" grep -q 'Error running command' "$WORK/$mode.log"
    check "$mode-clean" bash -c '! grep -E "BLOCK SHOULD BE STOPPED|pure virtual|stopped on an error that was not handled" "$1"' _ "$WORK/$mode.log"
    [ "$got" = 2 ] || cat "$WORK/$mode.log"
done
run 1 live-usage legacy live missing_pipeline "$WORK/usage" --source fork_probe --samplerate 6000000 --frequency 1701300000
run 3 live-radio legacy live metop_ahrpt "$WORK/radio" --source missing_driver --samplerate 6000000 --frequency 1701300000
exit $FAILED
