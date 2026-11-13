#!/bin/bash
# Fork: keep legacy-compatible VFO configs, rejecting malformed entries before radio opening.
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
# Fork: old startup read frequency as double, pipeline as string, and indexed parameters
# with string keys. Only an object or null (including a missing key) accepts those writes.
invalid = [('unknown', {'first': good, 'second': {**good, 'pipeline': 'missing_pipeline'}}),
           ('root-string', ''), ('root-number', 1), ('root-bool', True),
           ('entry-array', {'a': []}), ('entry-null', {'a': None}),
           ('entry-string', {'a': ''}), ('entry-number', {'a': 1}), ('entry-bool', {'a': True}),
           ('invalid-json', '{'), ('missing-file', None), ('numeric-flag', None)]
for key in ('frequency', 'pipeline'):
    missing = copy.deepcopy(good)
    del missing[key]
    invalid.append(('missing-' + key, {'a': missing}))
for key, values in [('frequency', ['1701300000', True, False, None, [], {}]),
                    ('pipeline', [9, True, False, None, [], {}, '', 'missing_pipeline']),
                    ('parameters', [[], 'text', 9, True, False])]:
    for index, value in enumerate(values):
        invalid.append((f'wrong-{key}-{index}', {'a': {**good, key: value}}))
missing_params = {key: value for key, value in good.items() if key != 'parameters'}
# Fork: positive cases must really open/start/stop the probe, and start every listed VFO.
valid = [('valid-unsigned-frequency', {'a': good}, 1701300000, 1),
         ('valid-missing-parameters', {'a': missing_params}, 1701300000, 1),
         ('valid-null-parameters', {'a': {**good, 'parameters': None}}, 1701300000, 1),
         ('valid-float-frequency', {'a': {**good, 'frequency': 1701300000.25}}, 1701300000, 1),
         ('valid-exponent-frequency', {'a': {**good, 'frequency': 1.7013e9}}, 1701300000, 1),
         ('valid-signed-frequency', {'a': {**good, 'frequency': -1}}, 0, 1),
         ('valid-extra-fields', {'a': {**good, 'label': 'ignored', 'parameters': {'unused_test_key': 42}}}, 1701300000, 1),
         ('valid-array', [good, missing_params, {**good, 'parameters': None}], 1701300000, 3),
         ('valid-empty-object', {}, 1701300000, 0),
         ('valid-empty-array', [], 1701300000, 0),
         ('valid-null-root', None, 1701300000, 0)]
cases = [(name, config, 1701300000, 0, False) for name, config in invalid]
cases += [(name, config, frequency, count, True) for name, config, frequency, count in valid]
failures = []
for name, config, frequency, count, accepted in cases:
    path, marker, log = (root / (name + suffix) for suffix in ('.json', '.opened', '.log'))
    if name != 'missing-file':
        payload = config if name == 'invalid-json' else json.dumps(config)
        if name == 'valid-exponent-frequency':
            payload = payload.replace('1701300000.0', '1.7013e9')
        path.write_text(payload)
    flag = '8080' if name == 'numeric-flag' else str(path)
    args = [sd, 'legacy', 'live', 'metop_ahrpt', str(root / name), '--source', 'fork_probe',
            '--samplerate', '6000000', '--frequency', str(frequency), '--timeout', '1', '--multi_vfo', flag]
    with log.open('w') as stream:
        p = subprocess.run(args, stdout=stream, stderr=subprocess.STDOUT, timeout=30,
                           env=dict(os.environ, SATDUMP_TEST_PROBE_OPENING=str(marker)))
    want = 0 if accepted else 1
    text = log.read_text()
    clean = not any(message in text for message in ('BLOCK SHOULD BE STOPPED', 'destroyed while running',
                                                    'pure virtual', 'stopped on an error that was not handled'))
    if accepted:
        lifecycle = ('Fork probe radio started' in text and 'Fork probe radio stopped' in text and
                     text.count('Added VFO for ') == count)
    else:
        lifecycle = 'Invalid multi-VFO configuration:' in text
    if p.returncode != want or marker.exists() != accepted or not lifecycle or not clean:
        failures.append(name)
        print(f'FAIL {name}: exit {p.returncode}, expected {want}; radio opened={marker.exists()}, '
              f'lifecycle={lifecycle}, clean={clean}\n{text[-1200:]}', flush=True)
    else:
        print(f'ok   {name} (exit {want}, radio opened={marker.exists()}, VFOs={count})', flush=True)
sys.exit(bool(failures))
PY
exit $FAILED
