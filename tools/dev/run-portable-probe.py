#!/usr/bin/env python3
"""Run a relocated isolated probe; never operate an installed DAW or service."""
import hashlib, json, os, pathlib, subprocess, sys, tempfile, time

bundle = pathlib.Path(sys.argv[1]).resolve()
mode = sys.argv[3] if len(sys.argv)>3 else 'monitor'
assert mode in ('monitor', 'capacity-ahead', 'capacity-direct')
destination = bundle.parent / ('coreaudio-run-' + str(time.time_ns()))
destination.mkdir()
private = destination / 'private-home'
private.mkdir()
empty = destination / 'empty'
empty.mkdir()
def digest(path): return hashlib.sha256(path.read_bytes()).hexdigest() if path.exists() else None
preferences = pathlib.Path.home() / 'Library/Preferences/Ardour9/config'
before = digest(preferences)
runtime = bundle / 'runtime'
manifest = json.loads((bundle / 'manifest.json').read_text())
for name, entry in manifest.items():
    if name.startswith('_'): continue
    assert digest(runtime / name)==entry['packaged_sha256'], 'Runtime hash mismatch: ' + name
env = {k: v for k, v in os.environ.items() if not k.startswith('ARDOUR_PROBE_')}
env.update(ARDOUR_PROBE_HOME=str(private),
           DYLD_INSERT_LIBRARIES=str(runtime / 'private-home.dylib'),
           DYLD_FALLBACK_LIBRARY_PATH=str(runtime),
           ARDOUR_DLL_PATH=str(runtime), ARDOUR_CONFIG_PATH=str(bundle / 'share'),
           ARDOUR_DATA_PATH=str(bundle / 'share'), ARDOUR_BACKEND_PATH=str(bundle / 'backends'),
           ARDOUR_PANNER_PATH=str(bundle / 'panners'), ARDOUR_SURFACES_PATH=str(empty),
           VAMP_PATH=str(empty), LV2_PATH=str(bundle / 'LV2'),
           ARDOUR_PROBE_BACKEND='CoreAudio',
           ARDOUR_PROBE_BLOCK=sys.argv[2] if len(sys.argv)>2 else '64')
if mode=='monitor':
    env.update(ARDOUR_PROBE_DISK_READER='1', ARDOUR_PROBE_MONITOR='1',
               ARDOUR_PROBE_RECORD_CONTROL='1', ARDOUR_PROBE_AUTOMATION='1', ARDOUR_PROBE_TAP='1')
else:
    env.update(ARDOUR_PROBE_CHAINS='8', ARDOUR_PROBE_INSERTS=sys.argv[4] if len(sys.argv)>4 else '32')
    if mode=='capacity-direct': env['ARDOUR_PROBE_DIRECT']='1'
started = time.monotonic()
with (destination / 'run.log').open('w') as log:
    process = subprocess.Popen([str(runtime / 'probe'), str(destination / 'session')],
                               env=env, stdout=log, stderr=subprocess.STDOUT)
    (destination / 'pid.txt').write_text(str(process.pid))
    try:
        status = process.wait(timeout=65)
    except subprocess.TimeoutExpired:
        process.terminate()
        try: process.wait(timeout=5)
        except subprocess.TimeoutExpired: process.kill(); process.wait()
        status = 'timeout'
assert digest(preferences)==before, 'User Ardour config changed'
output = (destination / 'run.log').read_text()
result = dict(exit_status=status, mode=mode, elapsed_seconds=time.monotonic()-started,
              user_config_unchanged=True, runtime_manifest=str(bundle / 'manifest.json'),
              runtime_manifest_sha256=digest(bundle / 'manifest.json'), runtime_hashes_verified=True,
              physical_loopback_latency_measured=False,
              settings={k:v for k,v in env.items() if k.startswith('ARDOUR_PROBE_')})
for line in output.splitlines():
    if line.startswith('RESULT '): result['probe_result']=json.loads(line[7:])
(destination / 'result.json').write_text(json.dumps(result, indent=2))
print(output)
print('EVIDENCE ' + str(destination), flush=True)
sys.exit(0 if status==0 and result.get('probe_result',{}).get('passed') else 1)
