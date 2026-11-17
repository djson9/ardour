#!/usr/bin/env python3
"""Test the actual MIDI Freeze context menu and Cancel dialog in the disposable fixture."""
import importlib.util,time,json,xml.etree.ElementTree as ET
from pathlib import Path
r=Path(__file__).resolve().parents[2];s=importlib.util.spec_from_file_location('m',r/'tools/dev/macos-fast-iterate.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m)
assert Path(m.tool('session_get_info')['sessionPath']).resolve()==(r/'verification-macbook/MIDI Freeze Verification').resolve()
track=json.loads((r/'artifacts/midi-freeze-verification.json').read_text())['trackId']
def plugin_state():
 m.tool('session_save')
 tree=ET.parse(r/'verification-macbook/MIDI Freeze Verification/MIDI Freeze Verification.ardour')
 route=next(t for t in tree.findall('./Routes/Route') if t.get('id')==track)
 return {p.get('id'):ET.tostring(p) for p in route.findall('Processor') if p.get('type')=='lv2'}
def state():return next(w for w in m.tool('ui_get_state')['windows'] if w['title'].endswith(' - Ardour'))
def menu():
 w=state();c=next(c for c in w['canvases'] if c['name']=='EditorMainCanvas');t=next(t for t in c['labels'] if 'Freeze Test Notes' in t['text']);box=t['ancestors'][0]
 for e in ['press','release']:m.tool('ui_canvas_pointer',window=state()['title'],canvas=c['name'],event=e,x=c['width']-5,y=box['y0']+35,button=3)
 m.tool('ui_press_key',window='',key='End');m.tool('ui_press_key',window='',key='Return')
if m.tool('track_get_info',id=track)['frozen']:m.tool('track_set_frozen',id=track,frozen=False)
live_plugins=m.tool('track_get_info',id=track)['plugins'];assert all(p['active'] for p in live_plugins)
menu();time.sleep(.4);assert m.tool('track_get_info',id=track)['frozen']
menu();time.sleep(.4);assert not m.tool('track_get_info',id=track)['frozen'];assert m.tool('track_get_info',id=track)['plugins']==live_plugins
m.tool('transport_locate_sample',sample=48000*3600);time.sleep(.1);m.tool('ui_activate_action',action='Common/set-session-end-from-playhead');time.sleep(.1);m.tool('transport_goto_start');time.sleep(.2)
original_plugin_state=plugin_state()
old=m.tool('track_get_regions',id=track);before=set((r/'verification-macbook/MIDI Freeze Verification').rglob('*.wav'));menu()
for _ in range(100):
 windows=m.tool('ui_get_state')['windows'];target=next((w for w in windows if 'Freeze' in w['title'] and not w['title'].endswith(' - Ardour')),None)
 if target:break
 time.sleep(.01)
else:raise RuntimeError('Freeze progress dialog not found')
m.tool('ui_press_key',window=target['title'],key='Escape')
for _ in range(100):
 if not any(w['title']==target['title'] for w in m.tool('ui_get_state')['windows']):break
 time.sleep(.05)
else:raise RuntimeError('Cancellation did not finish')
assert not m.tool('track_get_info',id=track)['frozen'];assert m.tool('track_get_regions',id=track)==old;assert m.tool('track_get_info',id=track)['plugins']==live_plugins
assert plugin_state()==original_plugin_state,'Cancellation changed plugin state'
new=set((r/'verification-macbook/MIDI Freeze Verification').rglob('*.wav'));assert new==before,(new-before)
m.tool('transport_locate_sample',sample=192000);time.sleep(.1);m.tool('ui_activate_action',action='Common/set-session-end-from-playhead');time.sleep(.1);m.tool('transport_goto_start');m.tool('session_save')
(r/'artifacts/midi-freeze-ui-cancel.json').write_text(json.dumps({'passed':True,'freezeAndUnfreezeViaContextMenu':True,'progressDialog':target['title'],'cancelled':True,'newWavFiles':0},indent=2)+'\n');print('UI freeze/unfreeze and cancellation passed')
