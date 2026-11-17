#!/usr/bin/env python3
"""Regression cases in the disposable MIDI Freeze Verification fixture."""
import importlib.util,json,time
from pathlib import Path
r=Path(__file__).resolve().parents[2];s=importlib.util.spec_from_file_location('m',Path(__file__).with_name('macos-fast-iterate.py'));m=importlib.util.module_from_spec(s);s.loader.exec_module(m)
assert Path(m.tool('session_get_info')['sessionPath']).resolve()==(r/'verification-macbook/MIDI Freeze Verification').resolve()
track=json.loads((r/'artifacts/midi-freeze-verification.json').read_text())['trackId'];report={}
m.tool('transport_stop')
for _ in range(100):
 if not m.tool('transport_get_state')['rolling']:
  # The engine still finishes declick/locate work after rolling becomes false.
  time.sleep(.3)
  if not m.tool('transport_get_state')['rolling']:break
 time.sleep(.02)
if m.tool('track_get_info',id=track)['frozen']:m.tool('track_set_frozen',id=track,frozen=False)
assert all(p['active'] for p in m.tool('track_get_info',id=track)['plugins'])
# Audio freeze must retain its original behavior.
before=m.tool('track_get_regions',id='488');assert m.tool('track_set_frozen',id='488',frozen=True)['frozen'];assert not m.tool('track_set_frozen',id='488',frozen=False)['frozen'];assert m.tool('track_get_regions',id='488')==before;report['audioFreezeRegression']=True
# Sends are rejected without mutating the MIDI track.
bus=next(t['id'] for t in m.tool('tracks_list')['tracks'] if t['type']=='bus' and t['name']!='Master')
send=m.tool('track_add_send',id=track,targetId=bus,db=-12);before=m.tool('track_get_info',id=track)
try:
 m.tool('track_set_frozen',id=track,frozen=True)
 raise AssertionError('Unsupported send accepted')
except RuntimeError as e:
 assert 'send' in str(e).lower(),e
after=m.tool('track_get_info',id=track);assert all(after[k]==before[k] for k in ['plugins','frozen','sends','fader'])
m.tool('track_remove_send',id=track,sendIndex=0,targetId=bus);report['routingRejection']=True
# Frozen playback must support loops, fader, and pan.
assert m.tool('track_set_frozen',id=track,frozen=True)['frozen']
m.tool('transport_loop_location_samples',startSample=0,endSample=192000);m.tool('transport_loop_toggle',enabled=True)
m.tool('transport_goto_start');m.tool('transport_play');samples=[]
for i in range(60):
 if i==15:m.tool('track_set_fader_db',id=track,db=-12)
 if i==30:m.tool('track_set_pan',id=track,position=0.0)
 if i==45:m.tool('track_set_fader_db',id=track,db=0)
 samples.append({'engine':m.tool('session_get_info')['audioEngine'],'track':m.tool('track_get_info',id=track),'transport':m.tool('transport_get_state')});time.sleep(.1)
m.tool('transport_loop_toggle',enabled=False);m.tool('transport_stop');m.tool('track_set_pan',id=track,position=.5)
for _ in range(100):
 if not m.tool('transport_get_state')['rolling']:
  # The engine still finishes declick/locate work after rolling becomes false.
  time.sleep(.3)
  if not m.tool('transport_get_state')['rolling']:break
 time.sleep(.02)
assert max(s['engine']['xruns'] for s in samples)==min(s['engine']['xruns'] for s in samples)
assert all(not p['active'] for s in samples for p in s['track']['plugins'])
assert any(max(s['track']['audioPeakDb'],default=-193)>-60 for s in samples)
assert any(samples[i]['transport']['sample']<samples[i-1]['transport']['sample'] for i in range(1,len(samples)))
assert not m.tool('track_set_frozen',id=track,frozen=False)['frozen'];assert abs(m.tool('track_get_info',id=track)['fader']['db']+6)<.01
m.tool('session_save');report['loopFaderPanPlayback']=samples
(r/'artifacts/midi-freeze-edge-validation.json').write_text(json.dumps(report,indent=2)+'\n');print('Audio regression, routing rejection and frozen loop/fader/pan passed')
