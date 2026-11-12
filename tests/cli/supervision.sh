#!/bin/bash
# Fork: the live command as the Controller watches it, through --http_server on 127.0.0.1:
#   - /api answers with the modules' state (JSON) while it runs, and shows the decoder locked
#   - SIGTERM ends it with exit code 0, and the .cadu written so far is kept: whole frames, each
#     starting with the sync marker
#   - SIGTERM while it is still starting (opening the radio) ends it the same way, once started
#   - a port another program already listens on ends it at once with exit code 1, saying why
#   - only the driver of the radio type asked for is asked for its radios (some drivers search
#     the network for radios when asked: UHD, PlutoSDR, SoapySDR)
# The radio is the file source playing a short made-up MetOp signal (cli/ahrpt_baseband.py) in a
# loop, in real time, or the test-only radio driver of tests/unit/probe_source_plugin.cpp, its
# file given as $2.
# Usage: supervision.sh <install prefix> <probe radio test plugin>
TESTS=$(cd "$(dirname "$0")/.." && pwd)
. "$TESTS/cli/common.sh"
PLUGIN=${2:?usage: $0 <install prefix> <probe radio test plugin>}
[ -f "$PLUGIN" ] || { echo "no test plugin at $PLUGIN"; exit 1; }

# Nothing listens on port 9 (discard) here: a download that came back by mistake goes nowhere
for v in http_proxy https_proxy HTTPS_PROXY ALL_PROXY all_proxy; do export "$v=http://127.0.0.1:9"; done
export no_proxy="" NO_PROXY=""

# 150 frames: 0.35 s of signal, played over and over
python3 -I -B "$TESTS/cli/ahrpt_baseband.py" "$WORK/ahrpt.cs8" 150 || exit 1
LIVE=(legacy live metop_ahrpt --source file --file_path "$WORK/ahrpt.cs8" --baseband_type cs8
    --samplerate 6000000 --frequency 1701300000)

free_port() {
    python3 -I -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])'
}
# Fetch http://127.0.0.1:<port><path> into a file; no proxy, 2 s at most
fetch() {
    python3 -I - "$1" "$2" > "$3" 2> /dev/null << 'PY'
import sys, urllib.request
opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
with opener.open(f"http://127.0.0.1:{sys.argv[1]}{sys.argv[2]}", timeout=2) as r:
    sys.stdout.write(r.read().decode())
PY
}
api_value() {
    python3 -I -c 'import json, sys; print(json.load(open(sys.argv[1]))["metop_ahrpt_decoder"]["deframer_state"])' "$1" 2> /dev/null
}
plain() { sed 's/\x1b\[[0-9;]*m//g' "$WORK/$1.log"; }
logged() { plain "$1" | grep -q "$2"; }
# wait_exit <pid> <name>: after a SIGTERM, the exit code must be 0 within 30 s
wait_exit() {
    local pid=$1 name=$2 waited=0
    while kill -0 "$pid" 2> /dev/null && [ $waited -lt 300 ]; do
        sleep 0.1
        waited=$((waited + 1))
    done
    if kill -0 "$pid" 2> /dev/null; then
        echo "FAIL $name: still running 30 s after SIGTERM"
        kill -KILL "$pid"
        FAILED=1
    fi
    wait "$pid"
    local got=$?
    if [ "$got" = 0 ]; then echo "ok   $name (exit 0)"; else echo "FAIL $name: exit $got, expected 0"; plain "$name" | tail -8 | sed 's/^/     /'; FAILED=1; fi
}
# Whole 1024-byte frames, at least one, each starting with 1ACFFC1D
whole_frames() {
    python3 -I -c 'import sys
d = open(sys.argv[1], "rb").read()
ok = len(d) > 0 and len(d) % 1024 == 0 and all(d[i:i + 4] == bytes.fromhex("1acffc1d") for i in range(0, len(d), 1024))
print(f"     {len(d) // 1024} frames of 1024 bytes" if ok else f"     {len(d)} bytes, not whole frames with the marker")
sys.exit(0 if ok else 1)' "$1"
}

# 1. Watched while it runs, locked on the signal, then stopped with SIGTERM
PORT=$(free_port)
"$SD" "${LIVE[@]:0:3}" "$WORK/live-1" "${LIVE[@]:3}" --http_server "127.0.0.1:$PORT" > "$WORK/sigterm.log" 2>&1 &
pid=$!
answered=0
for _ in $(seq 1 200); do
    if fetch "$PORT" /api "$WORK/api.json" && [ -s "$WORK/api.json" ]; then
        answered=1
        break
    fi
    sleep 0.1
done
check api-answers [ "$answered" = 1 ]
check api-is-json python3 -I -c 'import json, sys; d = json.load(open(sys.argv[1])); sys.exit(0 if isinstance(d, dict) and d else 1)' "$WORK/api.json"
check api-names-modules python3 -I -c 'import json, sys; d = json.load(open(sys.argv[1])); sys.exit(0 if "psk_demod" in d and "deframer_state" in d["metop_ahrpt_decoder"] else 1)' "$WORK/api.json"
state=
for _ in $(seq 1 150); do
    fetch "$PORT" /api "$WORK/api.json" && state=$(api_value "$WORK/api.json")
    [ "$state" = SYNCED ] && break
    sleep 0.1
done
check "api-shows-lock (deframer_state ${state:-none})" [ "$state" = SYNCED ]
sleep 1
check still-running kill -0 "$pid"

kill -TERM "$pid"
wait_exit "$pid" sigterm
check sigterm-logged logged sigterm "Signal Received. Stopping."
check cadu-whole-frames whole_frames "$WORK/live-1/metop_ahrpt.cadu"
check server-gone bash -c "! (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2> /dev/null"

# 2. The port is taken: exit code 1 at once, with the reason
PORT=$(free_port)
python3 -I -c 'import socket, sys, time
s = socket.socket(); s.bind(("127.0.0.1", int(sys.argv[1]))); s.listen(); print("ready", flush=True); time.sleep(120)' "$PORT" > "$WORK/holder.out" &
holder=$!
for _ in $(seq 1 50); do grep -q ready "$WORK/holder.out" 2> /dev/null && break; sleep 0.1; done
start=$(date +%s)
timeout -k 5 60 "$SD" "${LIVE[@]:0:3}" "$WORK/live-2" "${LIVE[@]:3}" --http_server "127.0.0.1:$PORT" > "$WORK/port-taken.log" 2>&1
got=$?
kill "$holder" 2> /dev/null
wait "$holder" 2> /dev/null
if [ "$got" = 1 ]; then echo "ok   port-taken (exit 1)"; else echo "FAIL port-taken: exit $got, expected 1"; plain port-taken | tail -8 | sed 's/^/     /'; FAILED=1; fi
check port-taken-reported logged port-taken "Cannot start the web server on 127.0.0.1:$PORT: the address is in use"
check port-taken-quick [ $(($(date +%s) - start)) -lt 20 ]

# 3. An address that is not host:port is the same error
run 1 bad-address "${LIVE[@]:0:3}" "$WORK/live-3" "${LIVE[@]:3}" --http_server "127.0.0.1:notaport" --timeout 20
check bad-address-reported logged bad-address "Cannot start the web server"

# The probe radio driver is loaded from ./plugins, as well as the installed ones
mkdir -p "$WORK/probe/plugins"
cp "$PLUGIN" "$WORK/probe/plugins/"
cd "$WORK/probe" || exit 1
export SATDUMP_TEST_PROBE_LISTED="$WORK/probe/listed" SATDUMP_TEST_PROBE_OPENING="$WORK/probe/opening"
export SATDUMP_TEST_PROBE_RELEASE="$WORK/probe/release"
PROBE=(legacy live metop_ahrpt "$WORK/live-probe" --source fork_probe --samplerate 6000000 --frequency 1701300000)

# 4. A run for the file source asks no other driver for its radios
run 0 file-only "${LIVE[@]:0:3}" "$WORK/live-4" "${LIVE[@]:3}" --timeout 1
check file-only-probe-not-asked test ! -e "$SATDUMP_TEST_PROBE_LISTED"
# ...while a run for the probe radio, from the same folder, does ask it (so the check above
# can fail)
touch "$SATDUMP_TEST_PROBE_RELEASE"
run 0 probe-asked "${PROBE[@]}" --timeout 1
check probe-asked-listed test -s "$SATDUMP_TEST_PROBE_LISTED"

# 5. SIGTERM while the radio is being opened: the run still starts, then stops with exit code 0
rm -f "$SATDUMP_TEST_PROBE_RELEASE" "$SATDUMP_TEST_PROBE_OPENING"
"$SD" "${PROBE[@]}" > "$WORK/early-sigterm.log" 2>&1 &
pid=$!
for _ in $(seq 1 300); do [ -e "$SATDUMP_TEST_PROBE_OPENING" ] && break; sleep 0.1; done
check early-sigterm-opening test -e "$SATDUMP_TEST_PROBE_OPENING"
kill -TERM "$pid"
sleep 0.5
touch "$SATDUMP_TEST_PROBE_RELEASE"
wait_exit "$pid" early-sigterm
check early-sigterm-logged logged early-sigterm "Signal Received. Stopping."

exit $FAILED
