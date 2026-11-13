#!/bin/bash
# Fork: review regressions: a joining radio, finite decimated samples, WAV and chained commands.
TESTS=$(cd "$(dirname "$0")/.." && pwd)
. "$TESTS/cli/common.sh"
PLUGIN=${2:?usage: $0 <install prefix> <probe radio test plugin>}
mkdir plugins
cp "$PLUGIN" plugins/ || exit 1
check record-review python3 -I - "$SD" "$WORK" <<'PY'
import array, os, pathlib, signal, struct, subprocess, sys, time
sd, work = sys.argv[1:]
root = pathlib.Path(work)
radio = ['--source', 'fork_probe', '--samplerate', '6000000', '--frequency', '1701300000', '--probe_stream']
failures = []
def run(name, args, want=0, sig=None, ready_size=None, env=None):
    log = root / (name + '.log')
    try:
        with log.open('w') as output:
            p = subprocess.Popen([sd, *args], stdout=output, stderr=subprocess.STDOUT, env=env)
            try:
                if sig:
                    path, size = ready_size
                    deadline = time.monotonic() + 45
                    while not (path.exists() and path.stat().st_size >= size):
                        if p.poll() is not None or time.monotonic() >= deadline:
                            raise RuntimeError('recording did not reach the required size')
                        time.sleep(0.01)
                    p.send_signal(sig)
                code = p.wait(timeout=20)
            finally:
                if p.poll() is None:
                    p.kill()
                p.wait()
        text = log.read_text()
        assert code == want, f'exit {code}, expected {want}'
        assert not any(s in text for s in ('BLOCK SHOULD BE STOPPED', 'pure virtual', 'stopped on an error that was not handled')), text
        print(f'ok   {name}', flush=True)
        return True
    except Exception as error:
        failures.append(name)
        print(f'FAIL {name}: {error}\n{log.read_text()[-1200:]}', flush=True)
        return False

def validate(name, fn):
    try:
        fn()
        print(f'ok   {name}', flush=True)
    except Exception as error:
        failures.append(name)
        print(f'FAIL {name}: {error}', flush=True)

# Fork: setup errors occur with a full source buffer and a producer already entering swap two.
for kind in ('record', 'live'):
    filled = root / (kind + '-filled')
    env = dict(os.environ, SATDUMP_TEST_PROBE_FILLED=str(filled))
    args = ['legacy', kind]
    if kind == 'live':
        args += ['metop_ahrpt']
    args += [str(root / ('partial-' + kind)), *radio, '--probe_fill_before_start', '--fft_enable',
             '--fft_size', 'invalid', '--baseband_format', 'cf32']
    run('joining-source-' + kind, args, 2, env=env)
    if not filled.exists():
        failures.append('source-not-filled-' + kind)
        print('FAIL source-not-filled-' + kind, flush=True)

# Fork: exactly 16 * 4096 input samples; no playback clock or scheduled stop determines length.
N = 16 * 4096
results = {}
for decimation in (1, 2, 4):
    for fft in (False, True):
        name = f'finite-D{decimation}-fft{int(fft)}'
        path = root / name
        args = ['legacy', 'record', str(path), *radio, '--baseband_format', 'cf32', '--decimation', str(decimation), '--probe_start_delay_ms', '2500', '--timeout', '60']
        if fft:
            args += ['--fft_enable']
        # Fork: processing may take longer than a short pass timeout on a shared machine.
        # Wait for all expected bytes, then stop, independently of wall-clock throughput.
        output = pathlib.Path(str(path) + '.cf32')
        if run(name, args, sig=signal.SIGTERM, ready_size=(output, N // decimation * 8)):
            def samples():
                data = pathlib.Path(str(path) + '.cf32').read_bytes()
                assert len(data) == N // decimation * 8, f'{len(data)} bytes, expected {N // decimation * 8}'
                values = array.array('f', data)
                if sys.byteorder != 'little':
                    values.byteswap()
                if decimation == 1:
                    for i in range(N):
                        assert values[2*i] == (i % 64 - 32) / 64
                        assert values[2*i+1] == (-0.5 if i % 2 else 0.5)
                else:
                    # Fork: decimation must remove the alternating (Nyquist) component; a source
                    # stream wired around the resampler has imaginary amplitude 0.5 instead.
                    assert max(abs(v) for v in values[257::2]) < 0.02
                    assert max(abs(v) for v in values[256::2]) > 0.1
                results[decimation, fft] = data
            validate(name + '-samples', samples)
    def same():
        assert results[decimation, False] == results[decimation, True], 'FFT changed recorded samples'
    validate(f'finite-D{decimation}-fft-identical', same)

def wav_check(path, decimation, complete):
    data = path.read_bytes()
    assert data[:4] == b'RIFF' and data[8:12] == b'WAVE' and data[36:40] == b'data'
    riff_size, = struct.unpack_from('<I', data, 4)
    fmt, channels, rate, byte_rate, align, bits = struct.unpack_from('<HHIIHH', data, 20)
    payload, = struct.unpack_from('<I', data, 40)
    assert (fmt, channels, rate, byte_rate, align, bits) == (1, 2, 6000000 // decimation, 24000000 // decimation, 4, 16)
    assert payload == len(data) - 44, (payload, len(data))
    assert payload % align == 0 and payload <= N // decimation * 4, payload
    if complete:
        assert payload == N // decimation * 4, payload
    assert riff_size == len(data) - 8, (riff_size, len(data))
    if payload:
        assert any(data[44:]), 'empty WAV payload'

# Fork: both timeout and signals must finalize WAV after the last payload write. Chained module
# commands restore default handlers; the following record must reinstall them for both signals.
for prefix in ([], ['module']):
    for sig in (None, signal.SIGTERM, signal.SIGINT):
        name = f'wav-{len(prefix)}-{sig.name if sig else "timeout"}'
        path = root / name
        wav = pathlib.Path(str(path) + '.wav')
        args = [*prefix, 'legacy', 'record', str(path), *radio, '--baseband_format', 'wav', '--decimation', '2', '--fft_enable', '--timeout', '60' if sig else '2']
        if run(name, args, sig=sig, ready_size=(wav, 44 + N // 2 * 4)):
            validate(name + '-header', lambda: wav_check(wav, 2, sig is not None))
# Fork: a timeout before the delayed source sends any samples still yields a consistent WAV.
path = root / 'wav-delayed-timeout'
if run('wav-delayed-timeout', ['legacy', 'record', str(path), *radio, '--baseband_format', 'wav',
                             '--decimation', '2', '--probe_start_delay_ms', '3000', '--timeout', '2']):
    validate('wav-delayed-timeout-header', lambda: wav_check(pathlib.Path(str(path) + '.wav'), 2, False))
sys.exit(bool(failures))
PY
exit $FAILED
