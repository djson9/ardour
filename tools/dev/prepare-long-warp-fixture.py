#!/usr/bin/env python3
"""Replace only the disposable MacBook test fixture with generated long audio."""
import importlib.util,os,signal,time,subprocess,plistlib,wave,math,array,copy,argparse
import xml.etree.ElementTree as ET
from pathlib import Path
ap=argparse.ArgumentParser();ap.add_argument('--stereo',action='store_true');args=ap.parse_args()
root=Path(__file__).resolve().parents[2]
s=importlib.util.spec_from_file_location('m',Path(__file__).with_name('macos-fast-iterate.py'));m=importlib.util.module_from_spec(s);s.loader.exec_module(m)
fixture=root/'verification-macbook/Warp Verification/Warp Verification.ardour'
assert Path(m.tool('session_get_info')['sessionPath']).resolve()==fixture.parent.resolve()
m.tool('transport_stop');m.tool('session_save')
backup=root/('artifacts/mono-fixture-before-stereo.ardour' if args.stereo else 'artifacts/short-fixture-before-streaming.ardour')
assert not backup.exists(),'Do not overwrite fixture backup'
backup.write_bytes(fixture.read_bytes())
app=Path.home()/'Applications/ArdourWarpTest.app'
for row in subprocess.check_output(['pgrep','-fl',str(app/'Contents/MacOS/Ardour9')],text=True).splitlines():
    pid=int(row.split()[0]);os.kill(pid,signal.SIGTERM)
    for _ in range(100):
        try:os.kill(pid,0)
        except ProcessLookupError:break
        time.sleep(.1)
    else:raise RuntimeError('Test app did not stop')
tree=ET.parse(fixture);session=tree.getroot();sources=session.find('Sources')
source_id=str(int(session.get('id-counter'))+100)
source=copy.deepcopy(sources[0]);source.set('id',source_id);source.set('name','warp-long-fixture.wav');sources.append(source)
audio=next(fixture.parent.rglob(sources[0].get('name'))).parent/source.get('name')
rate=48000;seconds=60;frames=rate*seconds
with wave.open(str(audio),'wb') as f:
    f.setnchannels(1);f.setsampwidth(2);f.setframerate(rate)
    one=array.array('h',(int(6000*math.sin(2*math.pi*220*i/rate)*(0.4+0.6*math.exp(-((i%12000)/1500)))) for i in range(rate)))
    for _ in range(seconds):f.writeframes(one.tobytes())
r=session.find('./Playlists/Playlist/Region')
for k,v in {'source-0':source_id,'master-source-0':source_id,'start':'a0','stretch':'1','shift':'1','ancestral-start':'a0','length':f'a{frames*5880}@a0','ancestral-length':f'a{frames*5880}@a0'}.items():r.set(k,v)
if args.stereo:
    right=copy.deepcopy(source);right.set('id',str(int(source_id)+1));right.set('name','warp-long-right.wav');sources.append(right)
    with wave.open(str(audio.parent/right.get('name')),'wb') as f:
        f.setnchannels(1);f.setsampwidth(2);f.setframerate(rate)
        one=array.array('h',(int(5000*math.sin(2*math.pi*330*i/rate)*(0.4+0.6*math.exp(-((i%16000)/2000)))) for i in range(rate)))
        for _ in range(seconds):f.writeframes(one.tobytes())
    r.set('source-1',right.get('id'));r.set('master-source-1',right.get('id'));r.set('channels','2')
    route=next(t for t in session.findall('.//Route') if t.get('id')=='488')
    io=next(t for t in route.findall('IO') if t.get('direction')=='Input')
    assert len(io.findall('Port'))==1
    port=copy.deepcopy(io[0]);port.set('name','Audio 1/audio_in 2');io.append(port)
    route.find('./Pannable/Controllable[@name="pan-width"]').set('value','1')
w=r.find('.//ClipWarp');w.set('reverse','0');w.set('bpm','120');w.set('mode','texture');w.set('pitch','0')
session.set('id-counter',str(int(source_id)+100))
tree.write(fixture,encoding='UTF-8',xml_declaration=True)
env=os.environ.copy();env.update(plistlib.loads((app/'Contents/Info.plist').read_bytes()).get('LSEnvironment',{}));env['ARDOUR_WARP_ENGINE_DIR']=str(root/'warp-engines-macbook')
with open(root/'artifacts/macbook-preview-launch.log','ab') as log:
    p=subprocess.Popen([str(app/'Contents/MacOS/Ardour9'),str(fixture)],env=env,stdout=log,stderr=log,start_new_session=True)
for _ in range(200):
    if p.poll() is not None:raise RuntimeError('Test app exited')
    try:
        assert Path(m.tool('session_get_info')['sessionPath']).resolve()==fixture.parent.resolve();break
    except Exception:time.sleep(.1)
else:raise RuntimeError('Fixture not ready')
m.tool('track_select',id='488');m.tool('ui_activate_action',action='Editor/select-all-objects');time.sleep(.2)
m.tool('ui_activate_action',action='Editor/zoom-to-extents');time.sleep(.2)
print(m.tool('track_get_regions',id='488'))
