#!/bin/bash
# A module that fails in its own thread must not end the program or hang it: the command
# reports the error and ends with exit code 2. So does a legacy command during which a DSP block
# failed. Uses the test plugin built from tests/unit/fault_plugin.cpp (its modules fail on
# purpose), given as $2.
# Usage: thread_faults.sh <install prefix> <fault test plugin>
. "$(dirname "$0")/common.sh"
PLUGIN=${2:?usage: $0 <install prefix> <fault test plugin>}
[ -f "$PLUGIN" ] || { echo "no test plugin at $PLUGIN"; exit 1; }

# Plugins and pipelines in the current folder are loaded as well as the installed ones
mkdir -p plugins
cp "$PLUGIN" plugins/
cat > pipelines.json <<'JSON'
{
    "fork_fault_good": {
        "name": "Fork fault test, no failure",
        "work": {
            "baseband": {},
            "soft": { "module": "fork_fault_a", "parameters": { "out_factor": 2 } },
            "frames": { "module": "fork_fault_b", "parameters": {} }
        }
    },
    "fork_fault_m1": {
        "name": "Fork fault test, first module fails",
        "work": {
            "baseband": {},
            "soft": { "module": "fork_fault_a", "parameters": { "fail_after": 2 } },
            "frames": { "module": "fork_fault_b", "parameters": {} }
        }
    },
    "fork_fault_m1_block": {
        "name": "Fork fault test, first module fails with its DSP block running",
        "work": {
            "baseband": {},
            "soft": { "module": "fork_fault_a", "parameters": { "fail_after": 2, "with_block": true } },
            "frames": { "module": "fork_fault_b", "parameters": {} }
        }
    },
    "fork_fault_blockfail": {
        "name": "Fork fault test, a DSP block of the first module fails",
        "work": {
            "baseband": {},
            "soft": { "module": "fork_fault_a", "parameters": { "block_fails": true } },
            "frames": { "module": "fork_fault_b", "parameters": {} }
        }
    },
    "fork_fault_m2": {
        "name": "Fork fault test, second module fails",
        "work": {
            "baseband": {},
            "soft": { "module": "fork_fault_a", "parameters": { "out_factor": 8 } },
            "frames": { "module": "fork_fault_b", "parameters": { "fail_after": 2 } }
        }
    },
    "fork_fault_live": {
        "name": "Fork fault test, live, second module fails",
        "live": [1, 2],
        "work": {
            "baseband": {},
            "soft": { "module": "fork_fault_a", "parameters": {} },
            "frames": { "module": "fork_fault_b", "parameters": { "fail_after": 3 } }
        }
    },
    "fork_fault_live_blockfail": {
        "name": "Fork fault test, live, a DSP block of the first module fails",
        "live": [1, 2],
        "work": {
            "baseband": {},
            "soft": { "module": "fork_fault_a", "parameters": { "block_fails": true } },
            "frames": { "module": "fork_fault_b", "parameters": {} }
        }
    },
    "fork_fault_live_good": {
        "name": "Fork fault test, live, no failure",
        "live": [1, 2],
        "work": {
            "baseband": {},
            "soft": { "module": "fork_fault_a", "parameters": {} },
            "frames": { "module": "fork_fault_b", "parameters": {} }
        }
    },
    "fork_fault_live_badinit": {
        "name": "Fork fault test, live, second module cannot be set up",
        "live": [1, 2],
        "work": {
            "baseband": {},
            "soft": { "module": "fork_fault_a", "parameters": {} },
            "frames": { "module": "fork_fault_b", "parameters": { "fail_init": true } }
        }
    }
}
JSON
cat > vfo.json <<'JSON'
{
    "v1": { "frequency": 100000000, "pipeline": "fork_fault_live_good", "parameters": {} },
    "v2": { "frequency": 100000000, "pipeline": "fork_fault_live_badinit", "parameters": {} }
}
JSON
cat > vfo_good.json <<'JSON'
{
    "v1": { "frequency": 100000000, "pipeline": "fork_fault_live_good", "parameters": {} }
}
JSON
head -c $((64 * 4096)) /dev/zero > in.bin
head -c $((100000 * 8)) /dev/zero > in.cf32

# Each run is limited, so a hang fails the test (exit 124, or 137 when SIGTERM is not enough)
# instead of stalling it. GNU timeout is used where there is one (gtimeout on macOS with coreutils)
SD_REAL=$SD
SD=timed_satdump
TIMEOUT=$(command -v timeout || command -v gtimeout || true)
timed_satdump() {
    if [ -n "$TIMEOUT" ]; then
        "$TIMEOUT" -k 5 30 "$SD_REAL" "$@"
        return
    fi
    "$SD_REAL" "$@" &
    local pid=$! waited=0
    while kill -0 "$pid" 2> /dev/null && [ $waited -lt 300 ]; do
        sleep 0.1
        waited=$((waited + 1))
    done
    if kill -0 "$pid" 2> /dev/null; then
        kill -TERM "$pid" 2> /dev/null
        sleep 5
        kill -KILL "$pid" 2> /dev/null
        wait "$pid" 2> /dev/null
        return 124
    fi
    wait "$pid"
}
logged() { sed 's/\x1b\[[0-9;]*m//g' "$WORK/$1.log" | grep -q "$2"; }
not_logged() { ! logged "$@"; }
size_of() { wc -c 2> /dev/null < "$1" | tr -d ' '; }

# Offline, two modules at once: the error comes back to the command
run 0 good pipeline fork_fault_good baseband in.bin out_good
check good-output [ "$(size_of out_good/fork_fault_good.out)" = 524288 ]
run 2 first-module-fails pipeline fork_fault_m1 baseband in.bin out_m1
check first-module-error-logged logged first-module-fails "Error running command.*failure on purpose"
run 2 second-module-fails pipeline fork_fault_m2 baseband in.bin out_m2
check second-module-error-logged logged second-module-fails "Error running command.*failure on purpose"
# A module that fails with its DSP block running is stopped before it goes (it used to be
# destroyed with the block running, which could end the program)
run 2 first-module-fails-with-block pipeline fork_fault_m1_block baseband in.bin out_m1_block
check first-module-block-stopped not_logged first-module-fails-with-block "destroyed while running"

# Live: a module that fails is reported at the end, and stopping does not hang
LIVE=(--source file --file_path in.cf32 --baseband_type cf32 --samplerate 1000000 --frequency 100000000 --timeout 3)
run 2 live-module-fails legacy live fork_fault_live out_live "${LIVE[@]}"
check live-module-error-logged logged live-module-fails "Live module fork_fault_b failed"
run 0 live-good legacy live fork_fault_live_good out_live_good "${LIVE[@]}"
check live-good-output [ -s out_live_good/fork_fault_live_good.out ]
# A DSP block that failed during a legacy run makes it a failed command (exit 2), live or not:
# the module carried on, but its output is incomplete. The legacy commands ended with 0
run 2 live-block-fails legacy live fork_fault_live_blockfail out_live_blockfail "${LIVE[@]}"
check live-block-fails-logged logged live-block-fails "Error running command : a DSP block failed.*block failure on purpose"
run 2 legacy-block-fails legacy fork_fault_blockfail baseband in.bin out_legacy_blockfail
check legacy-block-fails-logged logged legacy-block-fails "Error running command : a DSP block failed.*block failure on purpose"
# The same on one CPU with SCHED_BATCH, where a new thread does not take the CPU from the thread
# that made it: the block's thread then nearly always first runs after the module has stopped
# the block (the module reads its file in well under a millisecond). The fault module waits for
# its block to fail, so each run must still end with exit code 2. Without that wait, 19 of 20
# runs ended with 0; three runs make a lucky pass very unlikely. Linux only (taskset, chrt)
if [ "$(uname)" = Linux ]; then
    one_cpu=$(taskset -cp $$ | sed 's/.*: //; s/[-,].*//')
    one_cpu_satdump() { taskset -c "$one_cpu" chrt -b 0 "$TIMEOUT" -k 5 30 "$SD_REAL" "$@"; }
    SD=one_cpu_satdump
    for i in 1 2 3; do
        run 2 "legacy-block-fails-one-cpu-$i" legacy fork_fault_blockfail baseband in.bin "out_legacy_blockfail_one_cpu_$i"
    done
    SD=timed_satdump
else
    echo "note legacy-block-fails-one-cpu not run: it needs taskset and chrt (Linux)"
fi
# An error after the radio and the pipeline have stopped (--finish_processing given as a number,
# read only then) is reported with exit code 2. This checks only that; nothing known throws
# while everything runs, so the stop on such an error has no test here (multi-VFO has one:
# live-vfo-cannot-start)
run 2 live-error-after-start legacy live fork_fault_live_good out_live_error "${LIVE[@]}" --finish_processing 5
check live-error-after-start-logged logged live-error-after-start "Error running command"
# --http_server given as a number, or with no value, is not an address: like a server that cannot
# start, it stops everything that runs and ends with exit code 1 (it threw, and ended with 2)
run 1 live-http-number legacy live fork_fault_live_good out_live_http "${LIVE[@]}" --http_server 8080
check live-http-number-logged logged live-http-number "Cannot start the web server on 8080: "
check live-http-number-blocks-stopped not_logged live-http-number "BLOCK SHOULD BE STOPPED"
run 1 live-http-no-value legacy live fork_fault_live_good out_live_http "${LIVE[@]}" --http_server
check live-http-no-value-logged logged live-http-no-value "Cannot start the web server on true: "
# A pipeline that cannot start stops the FFT blocks it started (they were destroyed running)
run 1 live-cannot-start-fft legacy live fork_fault_live_badinit out_live_fft "${LIVE[@]}" --fft_enable
check live-cannot-start-fft-blocks-stopped not_logged live-cannot-start-fft "BLOCK SHOULD BE STOPPED"

# Multi VFO: a VFO that cannot start stops the ones already started, then the command ends
# Each VFO's frequency shifter is stopped too (they were destroyed running, on every way out but
# the normal end of the run)
run 2 live-vfo-cannot-start legacy live fork_fault_live_good out_vfo "${LIVE[@]}" --multi_vfo vfo.json
check live-vfo-error-logged logged live-vfo-cannot-start "init failure on purpose"
check live-vfo-cannot-start-blocks-stopped not_logged live-vfo-cannot-start "BLOCK SHOULD BE STOPPED"
run 1 live-vfo-http-number legacy live fork_fault_live_good out_vfo_http "${LIVE[@]}" --multi_vfo vfo_good.json --http_server 8080
check live-vfo-http-number-logged logged live-vfo-http-number "Cannot start the web server on 8080: "
check live-vfo-http-number-blocks-stopped not_logged live-vfo-http-number "BLOCK SHOULD BE STOPPED"
run 0 live-vfo-good legacy live fork_fault_live_good out_vfo_good "${LIVE[@]}" --multi_vfo vfo_good.json
check live-vfo-good-blocks-stopped not_logged live-vfo-good "BLOCK SHOULD BE STOPPED"

exit $FAILED
