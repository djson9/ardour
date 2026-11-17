#!/usr/bin/env python3
"""Update only the MacBook's separate warp test app from Middesk's packaged build."""
import importlib.util,os,signal,time,subprocess,plistlib
from pathlib import Path
root=Path(__file__).resolve().parents[2]
s=importlib.util.spec_from_file_location('m',Path(__file__).with_name('macos-fast-iterate.py'));m=importlib.util.module_from_spec(s);s.loader.exec_module(m)
fixture=root/'verification-macbook/Warp Verification/Warp Verification.ardour'
info=m.tool('session_get_info');assert Path(info['sessionPath']).resolve()==fixture.parent.resolve()
m.tool('transport_stop');m.tool('session_save')
app=Path.home()/'Applications/ArdourWarpTest.app'
for row in subprocess.check_output(['pgrep','-fl',str(app/'Contents/MacOS/Ardour9')],text=True).splitlines():
    pid=int(row.split()[0]);os.kill(pid,signal.SIGTERM)
    for _ in range(50):
        try:os.kill(pid,0)
        except ProcessLookupError:break
        time.sleep(.1)
    else:raise RuntimeError('Test app did not stop')
subprocess.run(['rsync','-a','davidson@100.95.4.81:/Users/davidson/Applications/ArdourWarpTest.app/',str(app)+'/'],check=True)
subprocess.run(['codesign','--force','--deep','--sign','-',str(app)],check=True)
subprocess.run(['codesign','--verify','--deep','--strict',str(app)],check=True)
env=os.environ.copy();env.update(plistlib.loads((app/'Contents/Info.plist').read_bytes()).get('LSEnvironment',{}));env['ARDOUR_WARP_ENGINE_DIR']=str(root/'warp-engines-macbook')
with open(root/'artifacts/macbook-preview-launch.log','ab') as log:
    p=subprocess.Popen([str(app/'Contents/MacOS/Ardour9'),str(fixture)],env=env,stdout=log,stderr=log,start_new_session=True)
for _ in range(100):
    if p.poll() is not None:raise RuntimeError('Test app exited')
    try:
        assert Path(m.tool('session_get_info')['sessionPath']).resolve()==fixture.parent.resolve()
        print('MacBook fixture ready:',p.pid);break
    except Exception:time.sleep(.1)
else:raise RuntimeError('Fixture not ready')
