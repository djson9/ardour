#!/usr/bin/env python3
"""Exercise clip Warp in a dedicated generated session via MCP.
Never runs against the user's music session.
"""
import importlib.util,json,time,sys
from pathlib import Path
spec=importlib.util.spec_from_file_location('iterate',Path(__file__).with_name('macos-fast-iterate.py'))
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
assert m.tool('session_get_info')['sessionName']=='Warp Verification', 'Dedicated fixture session required'

def region():
    regions=m.tool('track_get_regions',id='488')['regions'];assert len(regions)==1;return regions[0]
def window():
    return next(w['title'] for w in m.tool('ui_get_state')['windows'] if w['title'].endswith(' - Ardour'))
def controls_ready(control):
    deadline=time.monotonic()+5
    while time.monotonic()<deadline:
        controls=next(w for w in m.tool('ui_get_state')['windows'] if w['title'].endswith(' - Ardour')).get('warpControls',[])
        if any(c['id']=='warp-'+control for c in controls):return controls
        time.sleep(.05)
    raise AssertionError('Warp inspector did not restore control '+control)
def change(control,value,expected):
    old=region()['regionId']
    controls=controls_ready(control)
    current_value=next(c['value'] for c in controls if c['id']=='warp-'+control)
    if current_value==value:
        assert region()['lengthSamples']==expected
        return {'control':control,'unchanged':True,'region':region()}
    response=m.tool('ui_set_warp_control',control='warp-'+control,value=str(value).lower(),window=window())
    assert 'error' not in response,response
    started=time.monotonic()
    deadline=started+15
    while time.monotonic()<deadline:
        current=region()
        if current['regionId']!=old:
            assert current['lengthSamples']==expected,current
            controls=next(w for w in m.tool('ui_get_state')['windows'] if w['title'].endswith(' - Ardour')).get('warpControls',[])
            if not {'warp-enabled','warp-mode','warp-reverse','warp-bpm','warp-pitch','warp-cents'}.issubset({c['id'] for c in controls}):
                time.sleep(.05)
                continue
            assert next(c['value'] for c in controls if c['id']=='warp-'+control)==value, controls
            return {'control':control,'value':value,'region':current,'controls':controls,'elapsedSeconds':time.monotonic()-started}
        time.sleep(.1)
    raise AssertionError({'control':control,'state':m.tool('ui_get_state'),'logs':m.tool('ui_get_logs')})

m.tool('track_select',id='488');m.tool('ui_activate_action',action='Editor/select-all-objects');time.sleep(.4)
results=[]
controls=next(w for w in m.tool('ui_get_state')['windows'] if w['title'].endswith(' - Ardour'))['warpControls']
if not controls:
    m.tool('ui_activate_action',action='Editor/show-editor-props');controls=controls_ready('bpm')
initial_bpm=next(c['value'] for c in controls if c['id']=='warp-bpm')
for control,value,length in [('enabled',True,round(192000*initial_bpm/120)),('bpm',150,240000),('bpm',180,288000),('mode',5,288000),('mode',4,288000),('pitch',0,288000),('pitch',12,288000),('reverse',False,288000),('reverse',True,288000)]:
    results.append(change(control,value,length));print(control,'passed',flush=True)
if '--texture' in sys.argv:
    for control,value,length in [('mode',2,288000),('pitch',0,288000),('reverse',False,288000),('grain',30,288000),('flux',100,288000),('grain',65,288000),('flux',0,288000),('reverse',True,288000)]:
        results.append(change(control,value,length));print('texture',control,'passed',flush=True)
if '--repitch' in sys.argv:
    for control,value,length in [('mode',3,288000),('bpm',90,144000),('bpm',180,288000),('reverse',False,288000),('reverse',True,288000)]:
        results.append(change(control,value,length));print('repitch',control,'passed',flush=True)
    controls=controls_ready('pitch')
    assert all(not c['sensitive'] for c in controls if c['id'] in ('warp-pitch','warp-cents'))
m.tool('session_save')
if '--tones' in sys.argv:
    for control,value,length in [('mode',1,288000),('grain',30,288000),('grain',65,288000),('pitch',12,288000),('pitch',0,288000),('reverse',False,288000),('reverse',True,288000)]:
        results.append(change(control,value,length));print('tones',control,'passed',flush=True)
    controls=controls_ready('flux')
    assert not next(c['sensitive'] for c in controls if c['id']=='warp-flux')
    m.tool('session_save')
if '--beats' in sys.argv:
    for control,value in [('mode',0),('preserve',0),('loop',0),('loop',1),('loop',2),('transient-envelope',25),('transient-envelope',0),('transient-envelope',100),('preserve',1),('preserve',2),('preserve',3),('preserve',4),('preserve',0),('pitch',12),('pitch',0),('reverse',False),('reverse',True)]:
        results.append(change(control,value,288000));print('beats',control,'passed',flush=True)
    m.tool('session_save')
Path('artifacts').mkdir(exist_ok=True)
Path('artifacts/warp-integration-validation.json').write_text(json.dumps(results,indent=2)+'\n')
print(json.dumps({'passed':len(results),'session':'Warp Verification'}))
