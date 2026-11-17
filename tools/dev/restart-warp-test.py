#!/usr/bin/env python3
"""Install a prepared delta into the dedicated Middesk test app and reopen its fixture."""
import importlib.util,os,signal,time,subprocess,plistlib
from pathlib import Path
root=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('m',root/'tools/dev/macos-fast-iterate.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
assert m.tool('session_get_info')['sessionName']=='Warp Verification'
m.tool('transport_stop');m.tool('session_save')
app=Path.home()/'Applications/ArdourWarpTest.app'
subprocess.run(['python3',str(root/'tools/dev/macos-fast-iterate.py'),'prepare','--source',str(root),'--app',str(app)],check=True,stdout=subprocess.DEVNULL)
rows=subprocess.check_output(['pgrep','-fl',str(app/'Contents/MacOS/Ardour9')],text=True).splitlines()
for row in rows:
    pid=int(row.split()[0]);os.kill(pid,signal.SIGTERM)
    for _ in range(50):
        try:os.kill(pid,0)
        except ProcessLookupError:break
        time.sleep(.1)
    else:raise RuntimeError('Test app did not stop')
subprocess.run(['tar','xf',str(root/'build/fast-iterate/delta.tar'),'-C',str(app)],check=True)
subprocess.run(['codesign','--force','--deep','--sign','-',str(app)],check=True)
subprocess.run(['codesign','--verify','--deep','--strict',str(app)],check=True)
env=os.environ.copy();env.update(plistlib.loads((app/'Contents/Info.plist').read_bytes()).get('LSEnvironment',{}))
with open('/tmp/ardour-warp-launch.log','ab') as log:
    p=subprocess.Popen([str(app/'Contents/MacOS/Ardour9'),str(Path.home()/'workspace/ardour-test-sessions/Warp Verification/Warp Verification.ardour')],env=env,stdout=log,stderr=log,start_new_session=True)
for _ in range(100):
    if p.poll() is not None:raise RuntimeError('Test app exited')
    try:
        assert m.tool('session_get_info')['sessionName']=='Warp Verification'
        print('Ready:',p.pid);break
    except Exception:time.sleep(.1)
else:raise RuntimeError('Fixture did not become ready')
