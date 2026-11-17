#!/usr/bin/env python3
"""Promote the validated MacBook test app/engines with reversible local backups.
Launches only the disposable fixture; does not edit the original session.
"""
import hashlib,importlib.util,json,os,plistlib,re,shutil,signal,subprocess,time
from pathlib import Path
root=Path(__file__).resolve().parents[2]
s=importlib.util.spec_from_file_location('m',Path(__file__).with_name('macos-fast-iterate.py'));m=importlib.util.module_from_spec(s);s.loader.exec_module(m)
apps=Path.home()/'Applications';test=apps/'ArdourWarpTest.app';app=apps/'Ardour9.app'
fixture=root/'verification-macbook/Warp Verification/Warp Verification.ardour'
original=root/'verification/Warp Verification/Warp Verification.ardour'
assert Path(m.tool('session_get_info')['sessionPath']).resolve()==fixture.parent.resolve()
def digest(p):return hashlib.file_digest(p.open('rb'),'sha256').hexdigest()
def pids(path):return subprocess.run(['pgrep','-f','^'+re.escape(str(path/'Contents/MacOS/Ardour9'))],capture_output=True,text=True).stdout.split()
def stop(pid):
 os.kill(pid,signal.SIGTERM)
 for _ in range(100):
  try:os.kill(pid,0)
  except ProcessLookupError:return
  time.sleep(.1)
 raise RuntimeError('App did not stop')
assert not pids(app),'Main app is already running; do not replace it'
original_hash=digest(original)
stamp=time.strftime('%Y%m%dT%H%M%S');backups=root/'previous';backups.mkdir(exist_ok=True)
app_backup=backups/('Ardour9-before-warp-'+stamp+'.app');stage=apps/('.Ardour9-warp-'+stamp+'.app')
engines=Path.home()/'Library/Application Support/Ardour9/WarpEngines';engine_source=root/'warp-engines-macbook'
engine_backup=backups/('WarpEngines-before-'+stamp);engine_stage=engines.with_name('WarpEngines-staging-'+stamp)
for p in (stage,engine_stage,app_backup,engine_backup):assert not p.exists(),p
subprocess.run(['cp','-cR',str(test),str(stage)],check=True)
# Retain the user's installed colors/resources and launch preferences.
shutil.rmtree(stage/'Contents/Resources')
subprocess.run(['cp','-cR',str(app/'Contents/Resources'),str(stage/'Contents/Resources')],check=True)
shutil.copy2(app/'Contents/Info.plist',stage/'Contents/Info.plist')
subprocess.run(['codesign','--force','--deep','--sign','-',str(stage)],check=True)
subprocess.run(['codesign','--verify','--deep','--strict',str(stage)],check=True)
subprocess.run(['cp','-cR',str(engine_source),str(engine_stage)],check=True)
names=['ArdourWarpRender','OriginalEngineRender','OriginalTextureRender']
for mode in ['beats','tones','texture','repitch','complex','complex-pro']:names += [mode+'.dylib',mode+'.ready']
hashes={name:digest(engine_source/name) for name in names}
assert all(digest(engine_stage/name)==sha for name,sha in hashes.items())
assert all(os.access(engine_stage/name,os.X_OK) for name in names[:3])
manifest={'adapterProtocol':1,'sourceImageSha256':'5259706c37e9bba17f58154e8a3050cb7f62c06aefb4385d63d38ceadc4bd919','files':hashes}
(engine_stage/'installation.json').write_text(json.dumps(manifest,indent=2)+'\n')
m.tool('transport_stop');m.tool('session_save');running=pids(test);assert len(running)==1,running;stop(int(running[0]))
app.rename(app_backup)
if engines.exists():engines.rename(engine_backup)
stage.rename(app);engine_stage.rename(engines)
process=None
try:
 env=os.environ.copy();env.pop('ARDOUR_WARP_ENGINE_DIR',None);env.update(plistlib.loads((app/'Contents/Info.plist').read_bytes()).get('LSEnvironment',{}))
 with open(root/'artifacts/main-warp-launch.log','ab') as log:
  process=subprocess.Popen([str(app/'Contents/MacOS/Ardour9'),str(fixture)],env=env,stdout=log,stderr=log,start_new_session=True)
 deadline=time.monotonic()+20
 while time.monotonic()<deadline:
  if process.poll() is not None:raise RuntimeError('Installed app exited')
  try:
   info=m.tool('session_get_info');assert Path(info['sessionPath']).resolve()==fixture.parent.resolve();break
  except Exception:time.sleep(.1)
 else:raise RuntimeError('Installed app did not become ready')
 assert digest(original)==original_hash,'Original session changed'
 report={'pid':process.pid,'app':str(app),'appBackup':str(app_backup),'engines':str(engines),'engineBackup':str(engine_backup),'engineManifest':manifest,'session':info,'originalSession':str(original),'originalSessionSha256':original_hash}
 (root/'artifacts/warp-main-installation.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
except Exception:
 if process and process.poll() is None:stop(process.pid)
 app.rename(stage);app_backup.rename(app);engines.rename(engine_stage)
 if engine_backup.exists():engine_backup.rename(engines)
 raise
