#!/usr/bin/env python3
"""Supersede in-flight previews and abort before waiting for rendered output."""
import importlib.util,json,time,tempfile
from pathlib import Path
s=importlib.util.spec_from_file_location('m',Path(__file__).with_name('macos-fast-iterate.py'));m=importlib.util.module_from_spec(s);s.loader.exec_module(m)
info=m.tool('session_get_info');assert info['sessionName']=='Warp Verification'
def state():return next(w for w in m.tool('ui_get_state')['windows'] if w['title'].endswith(' - Ardour'))
def metrics():return m.tool('track_get_info',id='488')['warpPreview']
if state()['canvases'][0]['height']<300:
    m.tool('ui_activate_action',action='Editor/show-editor-props');time.sleep(.2)
root=Path(info['sessionPath']);files=set(root.rglob('*.wav'));tmp=Path(tempfile.gettempdir());scratch=set(tmp.glob('ardour-warp-*'))
before=m.tool('track_get_regions',id='488')['regions'];runs=[]
m.tool('transport_goto_start');m.tool('transport_play')
try:
    for iteration in range(5):
        c=next(c for c in state()['canvases'] if c['name']=='EditorMainCanvas');box=next(t for t in c['labels'] if t['text']=='Warp Fixture')['ancestors'][0]
        x=box['x1']-5;y=box['y1']-20
        def pointer(event,pos,down):return m.tool('ui_canvas_pointer',window=state()['title'],canvas=c['name'],event=event,x=pos,y=y,shift=True,buttonDown=down)
        pointer('move',x,False);pointer('press',x,False)
        for delta in (20,50,30,80,40,90):pointer('move',x-delta,True)
        started=time.monotonic();m.tool('ui_press_key',window=state()['title'],key='Escape')
        deadline=started+3
        while any(c['grabbed'] for c in state()['canvases']):
            assert time.monotonic()<deadline,'Drag did not abort';time.sleep(.01)
        pointer('release',x-90,True);time.sleep(.05)
        assert m.tool('track_get_regions',id='488')['regions']==before
        assert metrics()['activeGeneration']==0,metrics()
        runs.append({'iteration':iteration,'cancelMs':(time.monotonic()-started)*1000,'metrics':metrics()})
finally:m.tool('transport_stop')
assert set(root.rglob('*.wav'))==files
assert set(tmp.glob('ardour-warp-*'))<=scratch,sorted(str(p) for p in set(tmp.glob('ardour-warp-*'))-scratch)
after=m.tool('session_get_info');assert after['audioEngine']['xruns']==info['audioEngine']['xruns']
report={'initialSession':info,'finalSession':after,'runs':runs,'logs':m.tool('ui_get_logs')}
Path('artifacts/warp-rapid-cancel.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps({'passed':True,'runs':runs}))
