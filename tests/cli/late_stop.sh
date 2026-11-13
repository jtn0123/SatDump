#!/bin/bash
# Fork: deterministic stops between command entry and source open, with radio-open markers.
TESTS=$(cd "$(dirname "$0")/.." && pwd)
. "$TESTS/cli/common.sh"
LIVE=${2:?live harness}
RECORD=${3:?record harness}
PLUGIN=${4:?probe plugin}
FAULT_PLUGIN=${5:?fault module plugin}
mkdir plugins
cp "$PLUGIN" plugins/ || exit 1
printf '{}\n' > vfos.json
mkdir -p client/plugins
cp "$FAULT_PLUGIN" client/plugins/ || exit 1
cat > client/pipelines.json <<'JSON'
{"fork_client_stop":{"name":"Fork cancelled client","live":{"pkt_size":1024,"default":[1],"server":[1],"client":[1]},
"work":{"baseband":{},"bytes":{"module":"fork_fault_b","parameters":{"preserve_input":true}}}}}
JSON
export SATDUMP_TEST_PREFIX="$PREFIX"
export SATDUMP_TEST_PROBE_OPENING="$WORK/opened"
export SATDUMP_TEST_PROBE_LISTED="$WORK/listed"
for mode in single multi client record; do
    rm -f "$WORK/opened"
    args=(live metop_ahrpt "$WORK/$mode" --source fork_probe --samplerate 6000000 --frequency 1701300000 --timeout 1)
    exe="$LIVE"
    [ "$mode" = multi ] && args+=(--multi_vfo "$WORK/vfos.json")
    directory="$WORK"
    if [ "$mode" = client ]; then
        directory="$WORK/client"
        args=(live fork_client_stop "$WORK/client-out" --client --server_address 127.0.0.1 --server_port 1 --timeout 1)
    fi
    if [ "$mode" = record ]; then
        exe="$RECORD"
        args=(record "$WORK/record" --source fork_probe --samplerate 6000000 --frequency 1701300000 --baseband_format cs8 --timeout 1)
    fi
    (cd "$directory" && timeout -k 2 15 "$exe" "${args[@]}") > "$WORK/$mode.log" 2>&1
    got=$?
    check "$mode-exit-$got" test "$got" = 0
    check "$mode-injected" grep -q 'Injected stop after command entry' "$WORK/$mode.log"
    check "$mode-no-radio-open" test ! -e "$WORK/opened"
    # Client has no radio: its network source and modules must not be started either.
    check "$mode-no-source-start" bash -c '! grep -E "Opening TCP socket|Start processing|Fork probe radio started" "$1"' _ "$WORK/$mode.log"
done
# Fork: source discovery delivers a real stop after autotrack entry but before its constructor opens.
export SATDUMP_TEST_PROBE_STOP_ON_LIST=1
cat > autotrack.json <<JSON
{"parameters":{"source":"fork_probe","samplerate":6000000,"initial_frequency":1701300000},"output_folder":"$WORK/autotrack"}
JSON
rm -f "$WORK/opened" "$WORK/listed"
run 0 autotrack-late-stop legacy autotrack "$WORK/autotrack.json"
check autotrack-discovery-hook test -e "$WORK/listed"
check autotrack-no-radio-open test ! -e "$WORK/opened"
exit "$FAILED"
