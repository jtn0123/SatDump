#!/bin/bash
# Fork: signals throughout startup are remembered, without opening a radio for a cancelled pass.
TESTS=$(cd "$(dirname "$0")/.." && pwd)
. "$TESTS/cli/common.sh"
PLUGIN=${2:?usage: $0 <install prefix> <probe radio test plugin>}
mkdir plugins
cp "$PLUGIN" plugins/ || exit 1
python3 -I -B "$TESTS/cli/ahrpt_baseband.py" "$WORK/ahrpt.cs8" 150 || exit 1
check early-signals python3 -I - "$SD" "$WORK" <<'PY'
import os, pathlib, select, signal, subprocess, sys, time
sd, work = sys.argv[1:]
root = pathlib.Path(work)
radio = ['--source', 'file', '--file_path', str(root / 'ahrpt.cs8'), '--baseband_type', 'cs8',
         '--samplerate', '6000000', '--frequency', '1701300000', '--timeout', '30']
commands = {
    'live': ['legacy', 'live', 'metop_ahrpt', str(root / 'live'), *radio],
    'record': ['legacy', 'record', str(root / 'record'), *radio, '--baseband_format', 'cs8', '--decimation', '2'],
}
failures = []
def run(name, args, sig, delay, barrier=False):
    ready, release, opened = (root / part for part in ('ready', 'release', 'opened'))
    for path in (ready, release, opened):
        path.unlink(missing_ok=True)
    env = dict(os.environ)
    if barrier:
        env.update(SATDUMP_TEST_PROBE_INIT_READY=str(ready), SATDUMP_TEST_PROBE_INIT_RELEASE=str(release),
                   SATDUMP_TEST_PROBE_OPENING=str(opened))
    p = subprocess.Popen([sd, *args], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
    output = b''
    try:
        # Fork: the first banner byte proves main has begun. A signal before main (in exec or
        # the dynamic loader) cannot be caught by a handler installed in main. Delays start
        # here, including 0 ms; none is an exact scheduling requirement on a busy VM.
        if barrier:
            deadline = time.monotonic() + 25
            while not ready.exists():
                if p.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError('startup barrier was not reached')
                time.sleep(0.01)
        else:
            if not select.select([p.stdout], [], [], 25)[0]:
                raise RuntimeError('no startup banner')
            output += os.read(p.stdout.fileno(), 1)
            if not output:
                raise RuntimeError('exited before startup banner')
        time.sleep(delay / 1000)
        p.send_signal(sig)
        if barrier:
            release.touch()
        tail, _ = p.communicate(timeout=20)
        output += tail
        if p.returncode != 0:
            raise RuntimeError(f'exit {p.returncode}, expected 0')
        if b'stopped on an error that was not handled' in output or b'BLOCK SHOULD BE STOPPED' in output:
            raise RuntimeError('unclean shutdown')
        if barrier and opened.exists():
            raise RuntimeError('a cancelled run opened the radio')
        print(f'ok   {name}', flush=True)
    except Exception as error:
        failures.append(name)
        print(f'FAIL {name}: {error}', flush=True)
    finally:
        if p.poll() is None:
            p.kill()
        tail, _ = p.communicate()
        output += tail
        (root / (name + '.log')).write_bytes(output)
        if name in failures:
            print(output.decode(errors='replace')[-1800:], flush=True)

for sig in (signal.SIGTERM, signal.SIGINT):
    for round_no in range(3):
        for delay in (0, 10, 30, 60, 150):
            for kind, args in commands.items():
                run(f'{kind}-{sig.name}-{round_no}-{delay}ms', args, sig, delay)
    # Fork: hold plugin startup so these always exercise the request before command dispatch,
    # regardless of machine speed. A missing probe radio start would otherwise hide the bug.
    for kind in ('live', 'record'):
        args = ['legacy', kind]
        if kind == 'live':
            args += ['metop_ahrpt']
        args += [str(root / 'cancelled'), '--source', 'fork_probe', '--samplerate', '6000000',
                 '--frequency', '1701300000', '--baseband_format', 'cs8', '--timeout', '1']
        run(f'before-{kind}-{sig.name}', args, sig, 0, True)
    run(f'before-generic-{sig.name}', ['pipeline', 'metop_ahrpt', 'cadu', 'missing.cadu', 'unused'], sig, 0, True)
    run(f'before-autotrack-{sig.name}', ['legacy', 'autotrack', 'missing.json'], sig, 0, True)
    if (root / 'cancelled').exists() or (root / 'cancelled.cs8').exists():
        failures.append('cancelled-created-output')
        print('FAIL cancelled run created output', flush=True)
sys.exit(bool(failures))
PY
exit $FAILED
