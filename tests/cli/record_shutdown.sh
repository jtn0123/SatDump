#!/bin/bash
# Fork: every record exit must join the decimator before destroying its processing buffers.
TESTS=$(cd "$(dirname "$0")/.." && pwd)
. "$TESTS/cli/common.sh"
python3 -I -B "$TESTS/cli/ahrpt_baseband.py" "$WORK/ahrpt.cs8" 150 || exit 1
REC=(legacy record "$WORK/record" --source file --file_path "$WORK/ahrpt.cs8"
    --baseband_type cs8 --baseband_format cs8 --samplerate 6000000 --frequency 1701300000 --decimation 2)

bounded() {
    local want=$1 name=$2
    shift 2
    timeout -k 5 30 "$SD" "$@" > "$WORK/$name.log" 2>&1
    local got=$?
    check "$name (exit $got, expected $want)" test "$got" = "$want"
    check "$name-clean" bash -c '! grep -Ei "BLOCK SHOULD BE STOPPED|pure virtual|stopped on an error that was not handled" "$1"' _ "$WORK/$name.log"
    if [ "$got" != "$want" ]; then tail -12 "$WORK/$name.log"; fi
}
bounded 0 timeout "${REC[@]}" --timeout 2
check timeout-wrote test -s "$WORK/record.cs8"
rm -f "$WORK/record.cs8"
bounded 0 fft-timeout "${REC[@]}" --fft_enable --timeout 2
check fft-timeout-wrote test -s "$WORK/record.cs8"
bounded 1 webserver-error "${REC[@]}" --http_server 8080 --timeout 20
# Fork: also exercise a real bind failure after the resampler and sink have started.
check port-taken python3 -I - "$SD" "$WORK/port-taken.log" "${REC[@]}" <<'PYTEST'
import pathlib, socket, subprocess, sys
sd, log, *args = sys.argv[1:]
with socket.socket() as holder:
    holder.bind(('127.0.0.1', 0))
    holder.listen()
    with open(log, 'w') as stream:
        p = subprocess.run([sd, *args, '--http_server', f'127.0.0.1:{holder.getsockname()[1]}'],
                           stdout=stream, stderr=subprocess.STDOUT, timeout=30)
text = pathlib.Path(log).read_text()
assert p.returncode == 1, f'exit {p.returncode}, expected 1: {text}'
assert 'Cannot start the web server on 127.0.0.1:' in text, text
assert 'BLOCK SHOULD BE STOPPED' not in text, text
PYTEST
# Fork: this throws after the resampler starts, exercising exception cleanup as well as returns.
bounded 2 setup-error "${REC[@]}" --fft_enable --fft_size invalid --timeout 20
bounded 2 sink-setup-error "${REC[@]}" --fft_enable --baseband_format 123 --timeout 20
NO_FORMAT=(legacy record "$WORK/no-format" --source file --file_path "$WORK/ahrpt.cs8"
    --baseband_type cs8 --samplerate 6000000 --frequency 1701300000 --decimation 2)
bounded 1 missing-format "${NO_FORMAT[@]}" --timeout 20

# Fork: wait for actual output before stopping, so the signal exercises a running resampler.
for sig in TERM INT; do
    rm -f "$WORK/record.cs8"
    check "record-$sig" python3 -I - "$SD" "$WORK/record.cs8" "$WORK/$sig.log" "$sig" "${REC[@]}" --fft_enable <<'PY'
import pathlib, signal, subprocess, sys, time
sd, output, log, sig, *args = sys.argv[1:]
with open(log, 'w') as stream:
    p = subprocess.Popen([sd, *args], stdout=stream, stderr=subprocess.STDOUT)
    try:
        deadline = time.monotonic() + 25
        while not (pathlib.Path(output).exists() and pathlib.Path(output).stat().st_size > 0):
            if p.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError('recording never wrote samples')
            time.sleep(0.05)
        p.send_signal(getattr(signal, 'SIG' + sig))
        code = p.wait(timeout=20)
        if code != 0:
            raise RuntimeError(f'exit {code}, expected 0')
        text = pathlib.Path(log).read_text()
        if any(s in text for s in ('BLOCK SHOULD BE STOPPED', 'pure virtual', 'stopped on an error that was not handled')):
            raise RuntimeError(text)
    finally:
        if p.poll() is None:
            p.kill()
        p.wait()
PY
    check "record-$sig-wrote" test -s "$WORK/record.cs8"
done
exit $FAILED
