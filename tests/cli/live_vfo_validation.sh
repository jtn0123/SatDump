#!/bin/bash
# Fork: reject every unknown/malformed VFO before opening even the probe radio.
TESTS=$(cd "$(dirname "$0")/.." && pwd)
. "$TESTS/cli/common.sh"
PLUGIN=${2:?usage: $0 <install prefix> <probe radio test plugin>}
mkdir plugins
cp "$PLUGIN" plugins/ || exit 1
check vfo-validation python3 -I - "$SD" "$WORK" <<'PY'
import copy, json, os, pathlib, subprocess, sys
sd, work = sys.argv[1:]
root = pathlib.Path(work)
good = {'frequency': 1701300000, 'pipeline': 'metop_ahrpt', 'parameters': {}}
cases = [('unknown', {'first': good, 'second': {**good, 'pipeline': 'missing_pipeline'}}),
         ('root-array', []), ('root-null', None), ('entry-array', {'a': []}),
         ('entry-null', {'a': None}), ('invalid-json', '{'), ('missing-file', None),
         ('numeric-flag', None)]
for key in ('frequency', 'pipeline', 'parameters'):
    missing = copy.deepcopy(good)
    del missing[key]
    cases.append(('missing-' + key, {'a': missing}))
for key, value in [('frequency', '1701300000'), ('frequency', True), ('pipeline', 9),
                   ('pipeline', None), ('parameters', []), ('parameters', None)]:
    cases.append(('wrong-' + key + '-' + str(type(value).__name__), {'a': {**good, key: value}}))
cases.append(('valid', {'a': good}))
failures = []
for name, config in cases:
    path, marker, log = (root / (name + suffix) for suffix in ('.json', '.opened', '.log'))
    if name != 'missing-file':
        path.write_text(config if name == 'invalid-json' else json.dumps(config))
    flag = '8080' if name == 'numeric-flag' else str(path)
    args = [sd, 'legacy', 'live', 'metop_ahrpt', str(root / name), '--source', 'fork_probe',
            '--samplerate', '6000000', '--frequency', '1701300000', '--timeout', '1', '--multi_vfo', flag]
    with log.open('w') as stream:
        p = subprocess.run(args, stdout=stream, stderr=subprocess.STDOUT, timeout=30,
                           env=dict(os.environ, SATDUMP_TEST_PROBE_OPENING=str(marker)))
    want = 0 if name == 'valid' else 1
    text = log.read_text()
    if p.returncode != want or marker.exists() != (name == 'valid') or (
            name != 'valid' and 'Invalid multi-VFO configuration:' not in text):
        failures.append(name)
        print(f'FAIL {name}: exit {p.returncode}, expected {want}; radio opened={marker.exists()}\n{text[-1200:]}', flush=True)
    else:
        print(f'ok   {name} (exit {want}, radio opened={marker.exists()})', flush=True)
sys.exit(bool(failures))
PY
exit $FAILED
