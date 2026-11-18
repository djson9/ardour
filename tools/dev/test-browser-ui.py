#!/usr/bin/env python3
"""Run a <=9s browser check in an already-started, injected Verification app.
Compile browser_ui_probe.c as a GTK-linked dynamic library with dynamic lookup,
set ARDOUR_BROWSER_UI_PROBE to an isolated artifact folder and include it in
DYLD_INSERT_LIBRARIES. Startup/build are separate from the test deadline.
Run --restore after restarting the same private app to check persistence.
"""
import argparse, json, os, pathlib, signal, time, subprocess
p=argparse.ArgumentParser();p.add_argument('artifacts',type=pathlib.Path);p.add_argument('--restore',action='store_true');p.add_argument('--keys',action='store_true');p.add_argument('--pid',type=int,help='Private GUI process to activate for keyboard tests');args=p.parse_args()
if args.keys and not args.pid:p.error('--keys requires --pid for the private GUI app')
root=args.artifacts.resolve();root.mkdir(parents=True,exist_ok=True)
files=root/'files';files.mkdir(exist_ok=True)
if not args.restore:
    (files/'Nested').mkdir(exist_ok=True)
    for name in ['Piano.WaV','Bass.flac','Chords.MID','notes.txt','.hidden.wav','Nested/Inside.wav','Nested/notes.txt']:
        (files/name).touch()
    if not (files/'pipe.wav').exists():os.mkfifo(files/'pipe.wav')
result=root/'result.json';result.unlink(missing_ok=True)
key_request=root/'key-request';key_request.unlink(missing_ok=True)
def timeout(*_):raise TimeoutError('Browser GUI test exceeded 9 seconds')
signal.signal(signal.SIGALRM,timeout);signal.alarm(9)
start=time.monotonic()
if args.pid:
    subprocess.run(['osascript','-e',f'tell application "System Events" to tell (first application process whose unix id is {args.pid}) to set frontmost to true'],check=True,timeout=2)
(root/'trigger').write_text('restore' if args.restore else ('keys' if args.keys else 'full'))
keycodes={65364:125,65362:126,65361:123,65363:124,65360:115,65367:119,65293:36}
while not result.exists():
    if key_request.exists():
        keyval,modifiers=map(int,key_request.read_text().split())
        assert args.pid and keyval in keycodes
        shift=' using shift down' if modifiers & 1 else ''
        script=f'tell application "System Events" to tell (first application process whose unix id is {args.pid})\nset frontmost to true\nkey code {keycodes[keyval]}{shift}\nend tell'
        subprocess.run(['osascript','-e',script],check=True,timeout=2)
        key_request.unlink()
    time.sleep(.025)
# Producer closes this tiny file immediately; allow one retry if preempted.
try:data=json.loads(result.read_text())
except json.JSONDecodeError:time.sleep(.025);data=json.loads(result.read_text())
signal.alarm(0)
print(json.dumps(data));assert data['passed'],data
print(f'Elapsed: {time.monotonic()-start:.2f}s')
