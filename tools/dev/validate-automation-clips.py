#!/usr/bin/env python3
"""Run the native automation-clip regression with an enforced nine-second budget.
Build first: python3 waf build --targets=unit-test-automation-clips
On macOS, run this script once with --prepare to build process isolation.
"""
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import time

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--prepare', action='store_true')
args = parser.parse_args()
helper = root / 'artifacts/automation-clips/test-private-home.dylib'
helper.parent.mkdir(parents=True, exist_ok=True)
if args.prepare:
    if sys.platform != 'darwin':
        raise SystemExit('This isolated runner currently supports macOS.')
    subprocess.run(['clang', '-dynamiclib', str(root / 'tools/dev/test-private-home.c'),
                    '/opt/homebrew/opt/glib/lib/libglib-2.0.dylib', '-o', str(helper)], check=True)
    print('Prepared. Run again without --prepare for the bounded test.')
    raise SystemExit(0)
if not helper.is_file():
    raise SystemExit('Run with --prepare first.')
start = time.monotonic()
out = root / 'artifacts/automation-clips'
out.mkdir(parents=True, exist_ok=True)
# Read the build's environment without exposing it in logs.
command = 'TOP=' + shlex.quote(str(root)) + '; source "$TOP/build/gtk2_ardour/ardev_common_waf.sh"; env -0'
env = dict(x.split('=', 1) for x in subprocess.check_output(['bash', '-c', command], text=True).split('\0') if '=' in x)
private = tempfile.mkdtemp(prefix='home-', dir=out)
env.update(ARDOUR_PROBE_HOME=private, ARDOUR_PROBE_PRIVATE_SEMAPHORES='1', DYLD_INSERT_LIBRARIES=str(helper))
try:
    proc = subprocess.run([str(root / 'build/libs/ardour/test_automation_clips')], cwd=root,
                          env=env, capture_output=True, text=True,
                          timeout=max(.1, 9 - (time.monotonic() - start)))
    output = proc.stdout + proc.stderr
    passed = proc.returncode == 0 and 'OK (1)' in output and '[FATAL]' not in output
    result = dict(passed=passed, exit_code=proc.returncode, output=output)
except subprocess.TimeoutExpired:
    result = dict(passed=False, reason='nine-second deadline exceeded')
result['seconds'] = time.monotonic() - start
(out / 'test-result.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(result, indent=2))
raise SystemExit(0 if result['passed'] else 1)
