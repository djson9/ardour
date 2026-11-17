#!/usr/bin/env python3
"""Repeat the targeted feasibility matrix, retaining failures and exact provenance."""
import json, os, pathlib, shutil, subprocess, sys, time

root = pathlib.Path(__file__).resolve().parents[2]
build = pathlib.Path(sys.argv[1]).resolve()
destination = root / 'artifacts' / ('anticipation-validation-' + str(time.time_ns()))
destination.mkdir(parents=True)
arm = dict(DISK_READER=1, TAP=1, MONITOR=1, RECORD_CONTROL=1, AUTOMATION=1)
cases = [
    ('actual-arm-64-a', arm),
    ('actual-arm-64-b', arm),
    ('actual-arm-128', dict(arm, BLOCK=128)),
    ('held-midi-64', dict(MIDI=1)),
    ('held-midi-128-automation', dict(MIDI=1, BLOCK=128, AUTOMATION=1)),
    ('send-delay-187', dict(DISK=1, MONITOR=1, TAP=1, AUTOMATION=1, SEND_DELAY=187, BLOCK=128)),
]
cases = [(name, flags, 'anticipation_callback_probe.cc') for name, flags in cases]
cases += [
    ('main-output-sidechain-64', dict(MAIN_OUTPUT=1, SIDECHAIN=1, AUTOMATION=1), 'anticipation_graph_probe.cc'),
    ('main-output-sidechain-128', dict(MAIN_OUTPUT=1, SIDECHAIN=1, AUTOMATION=1, BLOCK=128), 'anticipation_graph_probe.cc'),
    ('automatic-pdc-64', dict(AUTOMATION=1, HOST_TIME=1), 'anticipation_pdc_probe.cc'),
    ('automatic-pdc-128', dict(AUTOMATION=1, BLOCK=128, HOST_TIME=1), 'anticipation_pdc_probe.cc'),
    ('actual-eligibility-controls', {}, 'anticipation_eligibility_probe.cc'),
    ('ongoing-disk-refill-64', dict(DISK_READER=1, TAP=1, AUTOMATION=1, REFILL=1), 'anticipation_callback_probe.cc'),
    ('ongoing-disk-refill-128', dict(DISK_READER=1, TAP=1, AUTOMATION=1, REFILL=1, BLOCK=128), 'anticipation_callback_probe.cc'),
    ('seek-edit-64', dict(AUTOMATION=1), 'anticipation_seek_probe.cc'),
    ('seek-edit-128', dict(AUTOMATION=1, BLOCK=128), 'anticipation_seek_probe.cc'),
]
results = []
for name, flags, probe in cases:
    env = {k: v for k, v in os.environ.items() if not k.startswith('ARDOUR_PROBE_')}
    env.update({'ARDOUR_PROBE_' + k: str(v) for k, v in flags.items()})
    with (destination / (name + '.log')).open('w') as log:
        completed = subprocess.run([sys.executable, str(root / 'tools/dev/run-anticipation-probe.py'),
                                    str(build), probe], env=env,
                                   stdout=log, stderr=subprocess.STDOUT, timeout=150)
    result = {'case': name, 'exit_code': completed.returncode}
    source = root / 'artifacts' / probe.replace('.cc', '').replace('_', '-') / 'result.json'
    if source.exists():
        shutil.copyfile(source, destination / (name + '.json'))
        data = json.loads(source.read_text())
        result.update(correctness_passed=data.get('correctness_passed', data.get('passed')),
                      max_sample_error=data.get('max_sample_error'),
                      cache_underruns=data.get('cache_underruns'),
                      measured_callback_budget_met=data.get('measured_callback_budget_met'))
    results.append(result)
    print(json.dumps(result), flush=True)
summary = {'correctness_passed': all(x.get('correctness_passed') and x['exit_code'] == 0 for x in results),
           'hardware_latency_validated': False, 'cases': results}
(destination / 'summary.json').write_text(json.dumps(summary, indent=2))
print('EVIDENCE ' + str(destination), flush=True)
sys.exit(0 if summary['correctness_passed'] else 1)
