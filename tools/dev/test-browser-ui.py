#!/usr/bin/env python3
"""Run a <=9s browser check in an already-started, injected Verification app.
Compile browser_ui_probe.c as a GTK-linked dynamic library with dynamic lookup,
set ARDOUR_BROWSER_UI_PROBE to an isolated artifact folder and include it in
DYLD_INSERT_LIBRARIES. Startup/build are separate from the test deadline.
Run --restore after restarting the same private app to check persistence.
"""
import argparse, json, os, pathlib, signal, time
p=argparse.ArgumentParser();p.add_argument('artifacts',type=pathlib.Path);p.add_argument('--restore',action='store_true');p.add_argument('--keys',action='store_true');args=p.parse_args()
root=args.artifacts.resolve();root.mkdir(parents=True,exist_ok=True)
files=root/'files';files.mkdir(exist_ok=True)
if not args.restore:
    (files/'Nested').mkdir(exist_ok=True)
    for name in ['Piano.WaV','Bass.flac','Chords.MID','notes.txt','.hidden.wav','Nested/Inside.wav','Nested/notes.txt']:
        (files/name).touch()
    if not (files/'pipe.wav').exists():os.mkfifo(files/'pipe.wav')
result=root/'result.json';result.unlink(missing_ok=True)
def timeout(*_):raise TimeoutError('Browser GUI test exceeded 9 seconds')
signal.signal(signal.SIGALRM,timeout);signal.alarm(9)
start=time.monotonic();(root/'trigger').write_text('restore' if args.restore else ('keys' if args.keys else 'full'))
while not result.exists():time.sleep(.025)
# Producer closes this tiny file immediately; allow one retry if preempted.
try:data=json.loads(result.read_text())
except json.JSONDecodeError:time.sleep(.025);data=json.loads(result.read_text())
signal.alarm(0)
print(json.dumps(data));assert data['passed'],data
print(f'Elapsed: {time.monotonic()-start:.2f}s')
