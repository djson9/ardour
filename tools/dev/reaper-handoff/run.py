from pathlib import Path
import subprocess, re, shutil, wave, math, struct, time, hashlib, argparse
args=argparse.ArgumentParser()
args.add_argument('--horizon',type=int,default=200)
args.add_argument('--block',type=int,default=64)
args.add_argument('--output',type=Path)
args.add_argument('--action',choices=['monitor','seek'],default='monitor')
args=args.parse_args()
source=Path(__file__).resolve().parent
out=args.output.resolve() if args.output else source
out.mkdir(parents=True,exist_ok=True)
(out/'action.txt').write_text(args.action)
if out!=source:
 for name in ['run.lua','trace.jsfx']:shutil.copy2(source/name,out/name)
resource=Path.home()/'Library/Application Support/REAPER'
config=resource/'reaper.ini'
before=hashlib.sha256(config.read_bytes()).hexdigest()
(out/'config/Effects').mkdir(parents=True,exist_ok=True)
(out/'plugins').mkdir(exist_ok=True)
shutil.copy2(out/'trace.jsfx',out/'config/Effects/trace')
values={'vstpath_arm64':str(out/'plugins'),'vst_scan':'0','coreaudioindevnew':'BlackHole 2ch','coreaudiooutdevnew':'BlackHole 2ch','coreaudiobs':str(args.block),'coreaudiobsuse':'1','coreaudiosrate':'48000','coreaudiosrateuse':'1','workerrender':'1','renderaheadlen':str(args.horizon)}
text='\n'.join(line for line in config.read_text().splitlines() if line.partition('=')[0] not in values and not line.startswith('lastproject='))
text,n=re.subn(r'^\[reaper\]$',lambda m:m.group()+'\n'+'\n'.join(k+'='+v for k,v in values.items()),text,count=1,flags=re.I|re.M)
assert n==1
(out/'config/reaper.ini').write_text(text+'\n')
with wave.open(str(out/'source.wav'),'wb') as f:
 f.setparams((1,2,48000,0,'NONE','not compressed'))
 f.writeframes(b''.join(struct.pack('<h',int(1000*math.sin(i*.017))) for i in range(48000*10)))
for n in ['complete.txt','error.txt','events.csv','trace-0.csv','trace-1.csv']:(out/n).unlink(missing_ok=True)
with (out/'reaper.log').open('w') as log:
 p=subprocess.Popen(['/Applications/REAPER.app/Contents/MacOS/REAPER','-newinst','-nosplash','-cfgfile',str(out/'config/reaper.ini'),'-new',str(out/'run.lua')],stdout=log,stderr=log)
 (out/'pid.txt').write_text(str(p.pid))
 try:
  for i in range(150):
   if (out/'error.txt').exists():raise RuntimeError((out/'error.txt').read_text())
   if (out/'complete.txt').exists():print((out/'complete.txt').read_text());break
   if p.poll() is not None:raise RuntimeError('REAPER exited '+str(p.returncode))
   time.sleep(.2)
  else:raise RuntimeError('Timeout; inspect isolated test instance')
 finally:
  if (out/'complete.txt').exists() or (out/'error.txt').exists():
   p.terminate();p.wait(timeout=10)
 assert hashlib.sha256(config.read_bytes()).hexdigest()==before,'Normal REAPER preferences changed'
 dest=out/'results'/f'{args.action}-{args.horizon}ms-{args.block}frames-{time.time_ns()}'
 dest.mkdir(parents=True)
 for n in ['complete.txt','events.csv','trace-0.csv','trace-1.csv','trace.rpp','action.txt']:
  shutil.copy2(out/n,dest/n)
 print('Evidence:',dest)
