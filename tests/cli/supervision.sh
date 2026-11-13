#!/bin/bash
# Fork: the live command as the Controller watches it, through --http_server on 127.0.0.1:
#   - /api answers with the modules' state (JSON) while it runs, and shows the decoder locked
#   - SIGTERM ends it with exit code 0 within 10 s (the grace FORK.md gives before SIGKILL), its
#     web server stops answering before it exits, and the .cadu written so far is kept: whole
#     frames, each starting with the sync marker
#   - SIGTERM while it is still starting (opening the radio) ends it the same way, once started
#   - a port another program already listens on, or a value that is not an address and port,
#     ends it at once with exit code 1, saying why ("Cannot start the web server on <value>:")
#   - only the driver of the radio type asked for is asked for its radios (some drivers search
#     the network for radios when asked: UHD, PlutoSDR, SoapySDR)
# legacy record is checked for the same web server errors and the same single-driver probe.
# The radio is the file source playing a short made-up MetOp signal (cli/ahrpt_baseband.py) in a
# loop, in real time, or the test-only radio driver of tests/unit/probe_source_plugin.cpp, its
# file given as $2.
# Usage: supervision.sh <install prefix> <probe radio test plugin> <fault test plugin>
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
# legacy record of the same signal; its output name goes after the first two
REC=(legacy record --source file --file_path "$WORK/ahrpt.cs8" --baseband_type cs8 --baseband_format cs8
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
now_ms() { echo $(($(date +%s%N) / 1000000)); }
# running <pid>: true only while the program runs. Its /proc/<pid>/stat can be read and does not
# show state Z (exited, not yet waited for). The state is the first field after the command
# name in brackets, which can itself hold spaces and brackets
running() {
    local stat
    { read -r stat < "/proc/$1/stat"; } 2> /dev/null || return 1
    stat=${stat##*) }
    [ "${stat%% *}" != Z ]
}
# wait_exit <pid> <name> <SIGTERM time from now_ms>: after a SIGTERM, the exit code must be 0
# within 10 s, the grace period FORK.md gives a station program before it sends SIGKILL. A run
# still there 30 s after this is called is killed
wait_exit() {
    local pid=$1 name=$2 start=$3 waited=0
    while kill -0 "$pid" 2> /dev/null && [ $waited -lt 300 ]; do
        sleep 0.1
        waited=$((waited + 1))
    done
    local took=$(($(now_ms) - start))
    if kill -0 "$pid" 2> /dev/null; then
        echo "FAIL $name: still running $((took / 1000)) s after SIGTERM"
        kill -KILL "$pid"
        FAILED=1
    elif [ $took -gt 10000 ]; then
        echo "FAIL $name: took $((took / 1000)) s to stop after SIGTERM, more than the 10 s a station program waits"
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

sigterm_at=$(now_ms)
kill -TERM "$pid"
# The server must stop answering while the program still runs (it closes the port itself). Once
# the program has exited the system closes the port anyway, so that would prove nothing. Bash
# can wait for a background program as soon as it exits, and its /proc/<pid> is then gone, so a
# missing /proc/<pid> proves nothing either. A refused connection counts only when the program
# is seen running after it: then it was running when the port refused. Tried for 30 s at most
server_closed=0
poll_end=$((SECONDS + 30))
while [ $SECONDS -lt $poll_end ] && running "$pid"; do
    if ! (exec 3<> "/dev/tcp/127.0.0.1/$PORT") 2> /dev/null; then
        running "$pid" && server_closed=1
        break
    fi
done
wait_exit "$pid" sigterm "$sigterm_at"
check sigterm-logged logged sigterm "Signal Received. Stopping."
check cadu-whole-frames whole_frames "$WORK/live-1/metop_ahrpt.cadu"
check server-closed-before-exit [ "$server_closed" = 1 ]

# 2. The port is taken: exit code 1 at once, with the reason
PORT=$(free_port)
python3 -I -c 'import socket, sys, time
s = socket.socket(); s.bind(("127.0.0.1", int(sys.argv[1]))); s.listen(); print("ready", flush=True); time.sleep(120)' "$PORT" > "$WORK/holder.out" &
holder=$!
for _ in $(seq 1 50); do grep -q ready "$WORK/holder.out" 2> /dev/null && break; sleep 0.1; done
start=$(date +%s)
timeout -k 5 60 "$SD" "${LIVE[@]:0:3}" "$WORK/live-2" "${LIVE[@]:3}" --http_server "127.0.0.1:$PORT" > "$WORK/port-taken.log" 2>&1
got=$?
if [ "$got" = 1 ]; then echo "ok   port-taken (exit 1)"; else echo "FAIL port-taken: exit $got, expected 1"; plain port-taken | tail -8 | sed 's/^/     /'; FAILED=1; fi
check port-taken-reported logged port-taken "Cannot start the web server on 127.0.0.1:$PORT: the address is in use"
check port-taken-quick [ $(($(date +%s) - start)) -lt 20 ]
# legacy record the same way (with --timeout, so a recording that goes on ends with exit 0)
run 1 record-port-taken "${REC[@]:0:2}" "$WORK/rec-2" "${REC[@]:2}" --http_server "127.0.0.1:$PORT" --timeout 20
check record-port-taken-reported logged record-port-taken "Cannot start the web server on 127.0.0.1:$PORT: the address is in use"
kill "$holder" 2> /dev/null
wait "$holder" 2> /dev/null

# 3. A value that is not an address and port is the same error: one with no number for the port,
# one that cannot be read as an address at all ("[::1"), and one that is not text (a number)
run 1 bad-address "${LIVE[@]:0:3}" "$WORK/live-3" "${LIVE[@]:3}" --http_server "127.0.0.1:notaport" --timeout 20
check bad-address-reported logged bad-address "Cannot start the web server on 127.0.0.1:notaport: "
run 1 unreadable-address "${LIVE[@]:0:3}" "$WORK/live-3" "${LIVE[@]:3}" --http_server "[::1" --timeout 20
check unreadable-address-reported logged unreadable-address "Cannot start the web server on \[::1: it is not an address and port"
run 1 number-address "${LIVE[@]:0:3}" "$WORK/live-3" "${LIVE[@]:3}" --http_server 8080 --timeout 20
check number-address-reported logged number-address "Cannot start the web server on 8080: it is not an address and port"
run 1 record-number-address "${REC[@]:0:2}" "$WORK/rec-3" "${REC[@]:2}" --http_server 8080 --timeout 20
check record-number-address-reported logged record-number-address "Cannot start the web server on 8080: it is not an address and port"

# The probe radio driver is loaded from ./plugins, as well as the installed ones
mkdir -p "$WORK/probe/plugins"
cp "$PLUGIN" "$WORK/probe/plugins/"
cd "$WORK/probe" || exit 1
export SATDUMP_TEST_PROBE_LISTED="$WORK/probe/listed" SATDUMP_TEST_PROBE_OPENING="$WORK/probe/opening"
export SATDUMP_TEST_PROBE_RELEASE="$WORK/probe/release"
PROBE=(legacy live metop_ahrpt "$WORK/live-probe" --source fork_probe --samplerate 6000000 --frequency 1701300000)

# 4. A run for the file source asks no other driver for its radios, live or record
run 0 file-only "${LIVE[@]:0:3}" "$WORK/live-4" "${LIVE[@]:3}" --timeout 1
check file-only-probe-not-asked test ! -e "$SATDUMP_TEST_PROBE_LISTED"
run 0 record-file-only "${REC[@]:0:2}" "$WORK/rec-4" "${REC[@]:2}" --timeout 1
check record-file-only-probe-not-asked test ! -e "$SATDUMP_TEST_PROBE_LISTED"
# ...while a run for the probe radio, from the same folder, does ask it (so the checks above
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
sigterm_at=$(now_ms)
kill -TERM "$pid"
sleep 0.5
touch "$SATDUMP_TEST_PROBE_RELEASE"
wait_exit "$pid" early-sigterm "$sigterm_at"
check early-sigterm-logged logged early-sigterm "Signal Received. Stopping."

# Fork: the network client must stop even when the server accepts TCP but never answers
# (not even the NNG handshake). A test pipeline uses the existing fault module as its reader.
mkdir -p "$WORK/client/plugins"
cp "${3:?usage: $0 <install prefix> <probe radio test plugin> <fault test plugin>}" "$WORK/client/plugins/" || exit 1
cd "$WORK/client" || exit 1
cat > pipelines.json <<'JSON'
{
    "fork_client_stop": {
        "name": "Fork silent client test",
        "live": { "pkt_size": 1024, "default": [1], "server": [1], "client": [1] },
        "work": {
            "baseband": {},
            "bytes": { "module": "fork_fault_b", "parameters": {} }
        }
    }
}
JSON
client_holder=
client_pid=
# Fork: also reap the listener and any hung client on an interrupted test.
trap 'if [ -n "$client_pid" ]; then kill -KILL "$client_pid" 2>/dev/null; wait "$client_pid" 2>/dev/null; fi; if [ -n "$client_holder" ]; then kill "$client_holder" 2>/dev/null; wait "$client_holder" 2>/dev/null; fi; rm -rf "$WORK"' EXIT
# Fork: keep a silent TCP peer and an idle NNG publisher. The latter completes only the
# protocol greeting, then sends no messages: that forces a wait in nng_recv, not just dial.
python3 -I -u -c 'import select, socket
def listener():
    s = socket.socket(); s.bind(("127.0.0.1", 0)); s.listen(); return s
silent, idle = listener(), listener()
print(silent.getsockname()[1], idle.getsockname()[1], flush=True)
clients = []
while True:
    for s in select.select([silent, idle], [], [])[0]:
        c, _ = s.accept(); clients.append(c)
        if s is idle:
            c.sendall(bytes.fromhex("0053500000200000"))
            print("idle", flush=True)
        else:
            print("silent", flush=True)' > "$WORK/silent.out" &
client_holder=$!
for _ in $(seq 1 300); do [ -s "$WORK/silent.out" ] && break; sleep 0.1; done
read -r CLIENT_PORT IDLE_PORT < "$WORK/silent.out"
CLIENT=(legacy live fork_client_stop "$WORK/client-output" --client --server_address 127.0.0.1)
for peer in silent idle; do
    port=$CLIENT_PORT
    [ "$peer" = idle ] && port=$IDLE_PORT
    name=client-$peer-sigterm
    "$SD" "${CLIENT[@]}" --server_port "$port" > "$WORK/$name.log" 2>&1 &
    client_pid=$!
    for _ in $(seq 1 300); do
        grep -q "^$peer$" "$WORK/silent.out" && break
        kill -0 "$client_pid" 2>/dev/null || break
        sleep 0.1
    done
    check "client-$peer-connected" grep -q "^$peer$" "$WORK/silent.out"
    sleep 1
    check "client-$peer-still-running" kill -0 "$client_pid"
    sigterm_at=$(now_ms)
    kill -TERM "$client_pid"
    wait_exit "$client_pid" "$name" "$sigterm_at"
    client_pid=
    check "$name-logged" logged "$name" "Signal Received. Stopping."
done

# Fork: client startup must also stop promptly on the same web server errors as radio mode.
# The silent listener also occupies an HTTP port, avoiding a free-port race.
for addr in "127.0.0.1:$CLIENT_PORT" 8080; do
    start=$(date +%s)
    name=client-http-$addr
    timeout -k 5 10 "$SD" "${CLIENT[@]}" --server_port "$CLIENT_PORT" --http_server "$addr" > "$WORK/$name.log" 2>&1
    got=$?
    check "$name-exit-1" [ "$got" = 1 ]
    check "$name-quick" [ $(($(date +%s) - start)) -lt 10 ]
    check "$name-reported" logged "$name" "Cannot start the web server on $addr: "
done
kill "$client_holder" 2>/dev/null
wait "$client_holder" 2>/dev/null
client_holder=

exit $FAILED
