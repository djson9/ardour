#!/usr/bin/env python3
"""Compare the saved disposable fixture against a direct original-engine render."""
import argparse,json,struct,subprocess,tempfile,xml.etree.ElementTree as ET
from pathlib import Path
import numpy as np
base=Path(__file__).resolve().parents[2]
ap=argparse.ArgumentParser();ap.add_argument('--session',type=Path,default=base/'verification-macbook/Warp Verification/Warp Verification.ardour');ap.add_argument('--engines',type=Path,default=base/'warp-engines-macbook');ap.add_argument('--source-start',type=int,default=0);ap.add_argument('--source-frames',type=int);args=ap.parse_args()
session=ET.parse(args.session).getroot();assert session.get('name')=='Warp Verification'
regions=session.findall('./Playlists/Playlist/Region');assert len(regions)==1
region=regions[0];settings=region.find('.//ClipWarp').attrib;channels=int(region.get('channels'))
sources={s.get('id'):s for s in session.findall('./Sources/Source')}
def read_wave(source_id):
    source=sources[source_id];path=next(args.session.parent.rglob(source.get('name')));raw=path.read_bytes();assert raw[:4]==b'RIFF'
    at=12;chunks={}
    while at+8<=len(raw):
        name,n=struct.unpack_from('<4sI',raw,at);chunks[name]=raw[at+8:at+8+n];at+=8+n+(n&1)
    fmt=chunks[b'fmt '];kind,ch,rate,_,_,bits=struct.unpack_from('<HHIIHH',fmt)
    if kind==65534:kind=struct.unpack_from('<H',fmt,24)[0]
    if kind==1 and bits==16:data=np.frombuffer(chunks[b'data'],dtype='<i2').astype(np.float32)/32768
    elif kind==3 and bits==32:data=np.frombuffer(chunks[b'data'],dtype='<f4')
    else:raise AssertionError((kind,bits))
    return data.reshape(-1,ch)[:,int(source.get('channel','0'))],rate
originals=[read_wave(region.get('master-source-'+str(ch))) for ch in range(channels)]
outputs=[read_wave(region.get('source-'+str(ch))) for ch in range(channels)]
rate=originals[0][1];assert all(r==rate for _,r in originals+outputs)
original=np.column_stack([d for d,_ in originals]);actual=np.column_stack([d for d,_ in outputs])
assert region.get('start')=='a0','This check expects a freshly rendered region'
assert region.get('ancestral-start')=='a'+str(args.source_start*5880),region.attrib
original=original[args.source_start:args.source_start+args.source_frames if args.source_frames is not None else None]
if args.source_frames is not None:assert len(original)==args.source_frames
with tempfile.TemporaryDirectory() as tmp:
    inp=Path(tmp)/'input.f32';out=Path(tmp)/'output.f32';original.tofile(inp)
    cmd=[str(args.engines/'ArdourWarpRender'),settings['mode'],str(inp),str(out),str(rate),str(channels),str(len(actual)/len(original))]+[settings[k] for k in ['pitch','formants','envelope','grain','flux','preserve','loop','transient-envelope']]
    run=subprocess.run(cmd,check=True,capture_output=True,text=True)
    expected=np.fromfile(out,dtype='<f4').reshape(-1,channels)
    if settings['reverse']=='1':expected=expected[::-1]
    assert actual.shape==expected.shape
    delta=np.abs(actual-expected);assert np.isfinite(actual).all() and not np.any(delta),float(delta.max())
    report={'session':str(args.session),'settings':settings,'channels':channels,'sourceStart':args.source_start,'sourceFrames':len(original),'frames':len(actual),'maxError':float(delta.max()),'peak':float(np.abs(actual).max()),'workerOutput':run.stdout}
path=base/'artifacts'/('warp-committed-'+settings['mode']+'-'+str(channels)+'ch.json');path.write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report))
