#!/usr/bin/env python3
"""Exercise freeze in the disposable MIDI Freeze Verification session only."""
import argparse, hashlib, importlib.util, json, time, urllib.request, xml.etree.ElementTree as ET
from pathlib import Path
s=importlib.util.spec_from_file_location('m',Path(__file__).with_name('macos-fast-iterate.py'));m=importlib.util.module_from_spec(s);s.loader.exec_module(m)
ap=argparse.ArgumentParser();ap.add_argument('phase',choices=['prepare','baseline','freeze','reload','unfreeze']);args=ap.parse_args()
root=Path(__file__).resolve().parents[2];fixture=root/'verification-macbook/MIDI Freeze Verification';report=root/'artifacts/midi-freeze-verification.json'
assert Path(m.tool('session_get_info')['sessionPath']).resolve()==fixture.resolve()
def call(tool_name,**kw):
 req=urllib.request.Request('http://127.0.0.1:4820/mcp',json.dumps({'jsonrpc':'2.0','id':1,'method':'tools/call','params':{'name':tool_name,'arguments':kw}}).encode(),{'Content-Type':'application/json'})
 with urllib.request.urlopen(req,timeout=70) as res:data=json.load(res)
 assert 'error' not in data,data
 data=data['result'];assert not data.get('isError'),data
 return data.get('structuredContent',data)
def sample(track):
 call('transport_goto_start');call('transport_play');out=[]
 for _ in range(20):
  time.sleep(.1);out.append({'engine':call('session_get_info')['audioEngine'],'track':call('track_get_info',id=track)})
 call('transport_stop');assert any(max(x['track'].get('audioPeakDb',[]),default=-193)>-60 for x in out), 'Playback was silent';return out
def snapshot(track):
 call('session_save');tree=ET.parse(fixture/'MIDI Freeze Verification.ardour');route=next(r for r in tree.findall('./Routes/Route') if r.get('id')==track)
 return {'route':ET.tostring(route).decode(),'regions':call('track_get_regions',id=track),'info':call('track_get_info',id=track),'midiHashes':{str(p.relative_to(fixture)):hashlib.sha256(p.read_bytes()).hexdigest() for p in fixture.rglob('*.mid')}}
if args.phase=='prepare':
 call('transport_stop');call('track_set_mute',id='488',value=True)
 before={t['id'] for t in call('tracks_list')['tracks']}
 call('tracks_add',type='midi',count=1,name='Freeze Synth',inputChannels=1,outputChannels=2)
 tracks=call('tracks_list')['tracks'];track=next(t['id'] for t in tracks if t['id'] not in before)
 available=call('plugin_list_available',search='ACE Reasonable Synth')['plugins'];assert available
 call('plugin_add_by_plugin_id',id=track,pluginId=available[0]['pluginId'])
 call('midi_note_import_json_to_new_region_samples',trackId=track,startSample=0,endSample=192000,name='Freeze Test Notes',midi={'midi_events':[{'bar':1,'b':1,'n':60,'v':100,'l':2},{'bar':1,'b':3,'n':64,'v':100,'l':2},{'bar':2,'b':1,'n':67,'v':100,'l':2}]})
 call('track_set_fader_db',id=track,db=-6)
 data={'trackId':track,'before':snapshot(track),'live':sample(track)}
 report.write_text(json.dumps(data,indent=2)+'\n');print('Prepared',track)
else:
 data=json.loads(report.read_text());track=data['trackId']
 if args.phase=='baseline':
  assert not call('track_get_info',id=track)['frozen']
  data['before']=snapshot(track);assert any(p['active'] for p in data['before']['info']['plugins']);data['live']=sample(track)
 elif args.phase=='freeze':
  state=call('track_set_frozen',id=track,frozen=True);assert state['frozen'],state
  assert all(not p['enabled'] for p in state['plugins']),state
  data['frozen']=snapshot(track);data['frozenPlayback']=sample(track)
  assert data['frozen']['regions']==data['before']['regions'],'Original MIDI changed'
  assert all(data['frozen']['midiHashes'].get(n)==h for n,h in data['before']['midiHashes'].items())
 elif args.phase=='reload':
  state=call('track_get_info',id=track);assert state['frozen'],state
  data['reloaded']=snapshot(track);data['reloadPlayback']=sample(track)
  assert all(data['reloaded']['midiHashes'].get(n)==h for n,h in data['before']['midiHashes'].items())
 else:
  state=call('track_set_frozen',id=track,frozen=False);assert not state['frozen'],state
  assert abs(state['fader']['db']+6)<.01,state
  assert call('track_get_regions',id=track)==data['before']['regions']
  assert state['plugins']==data['before']['info']['plugins'],(state['plugins'],data['before']['info']['plugins'])
  data['restored']=snapshot(track);data['restoredPlayback']=sample(track)
  old=ET.fromstring(data['before']['route']);new=ET.fromstring(data['restored']['route'])
  for proc in old.findall('Processor'):
   if proc.get('type')=='lv2':assert ET.tostring(proc)==ET.tostring(next(p for p in new.findall('Processor') if p.get('id')==proc.get('id')))
  assert all(data['restored']['midiHashes'].get(n)==h for n,h in data['before']['midiHashes'].items())
 report.write_text(json.dumps(data,indent=2)+'\n');print(args.phase,'passed')
