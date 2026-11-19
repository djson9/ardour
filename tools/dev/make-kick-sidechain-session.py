#!/usr/bin/env python3
"""Create a Kick -> Jay Compressor sidechain session and verify that it ducks.
Prepare separately: run-anticipation-probe.py BUILD_ROOT kick_sidechain_probe.cc --build-only
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--out', required=True, help='parent directory for the new session folder')
p.add_argument('--name', default='Kick Sidechain')
p.add_argument('--vst3-dir', default=str(Path.home() / 'Library/Audio/Plug-Ins/VST3'), help='directory containing Jay Compressor.vst3')
p.add_argument('--like', help='existing .ardour file whose engine hints and MCP server settings the new session should copy')
a = p.parse_args()
root = Path(__file__).resolve().parents[2]
prepared = root / 'artifacts/kick-sidechain-probe'
metadata = json.loads((prepared / 'prepared.json').read_text())
digest = lambda path: hashlib.sha256(Path(path).read_bytes()).hexdigest()
assert digest(prepared / 'probe') == metadata['probe_sha256'], 'Rebuild the probe'
assert digest(prepared / 'private-home.dylib') == metadata['isolation_sha256'], 'Rebuild isolation helper'
target = Path(a.out).resolve() / a.name
assert not target.exists(), f'{target} already exists'
build = Path(metadata['build_root'])
run = Path(tempfile.mkdtemp(prefix='session-', dir=prepared))
(run / 'home').mkdir()
env = os.environ.copy()
env.update(ARDOUR_PROBE_HOME=str(run / 'home'), ARDOUR_PROBE_PRIVATE_SEMAPHORES='1', LV2_PATH=str(build / 'build/libs/LV2'), VST3_PATH=str(Path(a.vst3_dir).resolve()))
script = 'TOP=' + shlex.quote(str(build)) + '\n. "$TOP/build/gtk2_ardour/ardev_common_waf.sh"\nexport DYLD_INSERT_LIBRARIES=' + shlex.quote(str(prepared / 'private-home.dylib')) + '\nexec ' + shlex.join([str(prepared / 'probe'), str(run), a.name])
with (run / 'run.log').open('w') as log:
    subprocess.run(['/bin/bash', '-c', script], env=env, stdout=log, stderr=subprocess.STDOUT, timeout=120)
text = (run / 'run.log').read_text()
# The dummy engine can crash during teardown after RESULT; the printed result is authoritative.
lines = [line for line in text.splitlines() if line.startswith('RESULT ')]
assert lines, text[-5000:]
result = json.loads(lines[0][7:])
assert result['passed'], json.dumps(result, indent=2)
snapshot = run / a.name / (a.name + '.ardour')
xml = snapshot.read_text()
if a.like:
    like = Path(a.like).read_text()
    for pattern in (r'<EngineHints [^>]*/>', r'<Protocol name="MCP HTTP Server \(Experimental\)"[^>]*/>'):
        found = re.search(pattern, like)
        if found: xml = re.sub(pattern, lambda _: found.group(0), xml, count=1)
snapshot.write_text(xml)
(run / a.name / (a.name + '.ardour.bak')).unlink(missing_ok=True)
target.parent.mkdir(parents=True, exist_ok=True)
shutil.copytree(run / a.name, target)
result.update(session=str(target / (a.name + '.ardour')), log=str(run / 'run.log'))
print(json.dumps(result))
