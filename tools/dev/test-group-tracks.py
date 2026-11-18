#!/usr/bin/env python3
"""Run one prepared native group test with a nine-second execution deadline.
Prepare separately: run-anticipation-probe.py BUILD_ROOT group_tracks_probe.cc --build-only
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--case', choices=['hierarchy', 'audio'], required=True)
a = p.parse_args()
start = time.monotonic()
root = Path(__file__).resolve().parents[2]
prepared = root / 'artifacts/group-tracks-probe'
metadata = json.loads((prepared / 'prepared.json').read_text())
digest = lambda path: hashlib.sha256(Path(path).read_bytes()).hexdigest()
assert digest(prepared / 'probe') == metadata['probe_sha256'], 'Rebuild the probe'
assert digest(prepared / 'private-home.dylib') == metadata['isolation_sha256'], 'Rebuild isolation helper'
for path, value in metadata['inputs'].items():
    assert digest(path) == value, 'Stale probe input; rebuild: ' + path
build = Path(metadata['build_root'])
run = Path(tempfile.mkdtemp(prefix='groups-', dir=prepared))
env = os.environ.copy()
env.update(ARDOUR_PROBE_HOME=str(run / 'home'), ARDOUR_PROBE_PRIVATE_SEMAPHORES='1', LV2_PATH=str(build / 'build/libs/LV2'))
(run / 'home').mkdir()
env.pop('GROUP_TRACK_AUDIO', None)
if a.case == 'audio': env['GROUP_TRACK_AUDIO'] = '1'
script = 'TOP=' + shlex.quote(str(build)) + '\n. "$TOP/build/gtk2_ardour/ardev_common_waf.sh"\nexport DYLD_INSERT_LIBRARIES=' + shlex.quote(str(prepared / 'private-home.dylib')) + '\nexec ' + shlex.join([str(prepared / 'probe'), str(run / 'session')])
with (run / 'run.log').open('w') as log:
    result = subprocess.run(['/bin/bash', '-c', script], env=env, stdout=log, stderr=subprocess.STDOUT, timeout=max(.1, 9 - (time.monotonic() - start)))
text = (run / 'run.log').read_text()
assert result.returncode == 0, text[-5000:]
result = json.loads(next(line[7:] for line in text.splitlines() if line.startswith('RESULT ')))
result['seconds'] = round(time.monotonic() - start, 3)
result['log'] = str(run / 'run.log')
assert result['passed'] and result['seconds'] < 9
print(json.dumps(result))
