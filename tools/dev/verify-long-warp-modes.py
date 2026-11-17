#!/usr/bin/env python3
import importlib.util,time,subprocess,json,sys
from pathlib import Path
s=importlib.util.spec_from_file_location('m',Path(__file__).with_name('macos-fast-iterate.py'));m=importlib.util.module_from_spec(s);s.loader.exec_module(m)
assert m.tool('session_get_info')['sessionName']=='Warp Verification'
assert m.tool('track_get_regions',id='488')['regions'][0]['lengthSamples']==2880000
for mode,name in [(0,'beats'),(1,'tones'),(2,'texture'),(3,'repitch'),(4,'complex'),(5,'complex-pro')]:
    def state():return next(w for w in m.tool('ui_get_state')['windows'] if w['title'].endswith(' - Ardour'))
    m.tool('track_select',id='488');m.tool('ui_activate_action',action='Editor/select-all-objects');time.sleep(.2)
    if not state().get('warpControls'):
        m.tool('ui_activate_action',action='Editor/show-editor-props');time.sleep(.3)
    m.tool('ui_set_warp_control',window=state()['title'],control='warp-mode',value=str(mode))
    deadline=time.monotonic()+20
    while time.monotonic()<deadline:
        controls=state().get('warpControls',[])
        if any(c['id']=='warp-mode' and c['value']==mode for c in controls):break
        time.sleep(.1)
    else:raise RuntimeError('mode did not apply: '+name)
    time.sleep(.6)
    p=subprocess.run([sys.executable,str(Path(__file__).with_name('verify-warp-drag.py')),'--hold-seconds','4','--cancel'])
    report=Path('artifacts/warp-drag-live-validation.json')
    if report.exists():Path('artifacts/warp-drag-streaming-long-'+name+'.json').write_bytes(report.read_bytes())
    if p.returncode:raise RuntimeError(name+' drag failed')
    print(name+' passed',flush=True)
