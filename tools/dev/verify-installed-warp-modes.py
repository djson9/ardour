#!/usr/bin/env python3
"""Validate all installed modes against direct stereo original-source renders."""
import argparse,importlib.util,json,subprocess,time
from pathlib import Path
ap=argparse.ArgumentParser();ap.add_argument('--python',required=True);args=ap.parse_args()
root=Path(__file__).resolve().parents[2]
s=importlib.util.spec_from_file_location('m',Path(__file__).with_name('macos-fast-iterate.py'));m=importlib.util.module_from_spec(s);s.loader.exec_module(m)
info=m.tool('session_get_info');assert Path(info['sessionPath']).resolve()==(root/'verification-macbook/Warp Verification').resolve()
def region():return m.tool('track_get_regions',id='488')['regions'][0]
def state():return next(w for w in m.tool('ui_get_state')['windows'] if w['title'].endswith(' - Ardour'))
before=region();assert before['lengthSamples']==2880000
m.tool('track_select',id='488');m.tool('ui_activate_action',action='Editor/select-all-objects');time.sleep(.3)
if not state().get('warpControls'):m.tool('ui_activate_action',action='Editor/show-editor-props');time.sleep(.3)
changes=0

def change(control,value):
 global changes
 controls=state()['warpControls']
 if next(c['value'] for c in controls if c['id']=='warp-'+control)==value:return
 old=region()['regionId'];m.tool('ui_set_warp_control',window=state()['title'],control='warp-'+control,value=str(value).lower())
 deadline=time.monotonic()+20
 while time.monotonic()<deadline:
  if region()['regionId']!=old and state().get('warpControls'):break
  time.sleep(.1)
 else:raise RuntimeError('Render did not finish: '+control)
 changes+=1
 time.sleep(.1)

results=[]
change('bpm',123)
for mode,name in enumerate(['beats','tones','texture','repitch','complex','complex-pro']):
 change('mode',mode);m.tool('session_save')
 assert region()['lengthSamples']==2952000,region()
 output=subprocess.check_output([args.python,str(Path(__file__).with_name('verify-warp-committed-audio.py')),'--engines',str(Path.home()/'Library/Application Support/Ardour9/WarpEngines')],text=True)
 comparison=json.loads(output);assert comparison['settings']['mode']==name and comparison['maxError']==0 and comparison['channels']==2
 results.append(comparison);print(name+' exact stereo output passed',flush=True)
def assert_canvas_matches_playlist():
 deadline=time.monotonic()+3
 while time.monotonic()<deadline:
  labels=[t for c in state()['canvases'] if c['name']=='EditorMainCanvas' for t in c['labels'] if t['text']=='Warp Fixture']
  if len(labels)==1:return
  time.sleep(.05)
 raise AssertionError({'playlist':m.tool('track_get_regions',id='488'),'canvasLabels':labels})
m.tool('session_undo',count=changes);assert region()==before,(region(),before)
assert_canvas_matches_playlist()
m.tool('session_redo',count=changes);assert_canvas_matches_playlist()
m.tool('session_undo',count=changes);assert region()==before,(region(),before);assert_canvas_matches_playlist();m.tool('session_save')
report={'session':info,'changesUndone':changes,'cases':results,'restored':region()}
(root/'artifacts/warp-installed-all-modes.json').write_text(json.dumps(report,indent=2)+'\n')
print('All six installed modes passed; fixture restored.')
