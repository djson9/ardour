#!/usr/bin/env python3
"""Bounded playback test against the already-started private injected GUI."""
import argparse,json,pathlib,signal,time,wave,math,array
p=argparse.ArgumentParser();p.add_argument('artifacts',type=pathlib.Path);p.add_argument('--prepare',action='store_true');p.add_argument('--restore',action='store_true');args=p.parse_args()
root=args.artifacts.resolve();root.mkdir(parents=True,exist_ok=True)
if args.prepare:
    files=root/'files';files.mkdir(exist_ok=True)
    for name,amplitude,hz in [('quiet',.05,440),('alternate',.4,220),('latest',.2,880)]:
        pcm=array.array('h')
        for i in range(8*48000):
            value=int(32767*amplitude*(.3+.7*math.sin(math.pi*i/(8*48000))**2)*math.sin(2*math.pi*hz*i/48000))
            pcm.extend([value,value//2])
        with wave.open(str(files/(name+'.wav')),'wb') as f:f.setnchannels(2);f.setsampwidth(2);f.setframerate(48000);f.writeframes(pcm.tobytes())
    print('Prepared 8-second audio fixtures');raise SystemExit

def timeout(*_):raise TimeoutError('Browser waveform GUI exceeded 9 seconds')
signal.signal(signal.SIGALRM,timeout);signal.alarm(9);started=time.monotonic()
result=root/'result.json';result.unlink(missing_ok=True);(root/'trigger').write_text('restore' if args.restore else 'full')
while not result.exists():time.sleep(.025)
try:data=json.loads(result.read_text())
except json.JSONDecodeError:time.sleep(.025);data=json.loads(result.read_text())
print(json.dumps(data));assert data['passed'],data
signal.alarm(0);print(f'Elapsed: {time.monotonic()-started:.2f}s')
