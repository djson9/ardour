#!/usr/bin/env python3
"""Compare a saved, untrimmed Texture fixture to its original-source render."""
import json,struct,subprocess,tempfile,xml.etree.ElementTree as ET
from pathlib import Path
import numpy as np
root=Path.home()/'workspace/ardour-test-sessions/Warp Verification'
session=ET.parse(root/'Warp Verification.ardour').getroot()
regions=session.findall('./Playlists/Playlist/Region');assert len(regions)==1
region=regions[0];settings=region.find('.//ClipWarp').attrib
assert settings['mode'] in ('texture','repitch','tones','beats') and region.get('start')=='a0'
sources={s.get('id'):s.get('name') for s in session.findall('./Sources/Source')}
def read_wave(source_id):
    path=next(root.rglob(sources[source_id]));raw=path.read_bytes();assert raw[:4]==b'RIFF'
    at=12;chunks={}
    while at+8<=len(raw):
        name,n=struct.unpack_from('<4sI',raw,at);chunks[name]=raw[at+8:at+8+n];at+=8+n+(n&1)
    fmt=chunks[b'fmt '];kind,ch,rate,_,_,bits=struct.unpack_from('<HHIIHH',fmt)
    if kind==65534:kind=struct.unpack_from('<H',fmt,24)[0]
    if kind==1 and bits==16:data=np.frombuffer(chunks[b'data'],dtype='<i2').astype(np.float32)/32768
    elif kind==3 and bits==32:data=np.frombuffer(chunks[b'data'],dtype='<f4')
    else:raise AssertionError((kind,bits))
    return data.reshape(-1,ch),rate
original,rate=read_wave(region.get('master-source-0'));actual,actual_rate=read_wave(region.get('source-0'))
assert rate==actual_rate
engines=Path.home()/'Library/Application Support/Ardour9/WarpEngines'
with tempfile.TemporaryDirectory() as tmp:
    inp=Path(tmp)/'input.f32';out=Path(tmp)/'output.f32';original.tofile(inp)
    args=[str(engines/'OriginalTextureRender'),str(engines/(settings['mode']+'.dylib')),str(inp),str(out),str(rate),str(original.shape[1]),region.get('stretch'),settings['pitch'],settings['grain'],settings['flux']]
    if settings['mode']=='beats':args += [settings['preserve'],settings['loop'],settings['transient-envelope']]
    args += ['--mode='+settings['mode']]
    run=subprocess.run(args,check=True,capture_output=True,text=True)
    expected=np.fromfile(out,dtype='<f4').reshape(-1,original.shape[1])
    if settings['reverse']=='1':expected=expected[::-1]
    assert actual.shape==expected.shape
    delta=np.abs(actual-expected);assert np.isfinite(actual).all() and not np.any(delta)
    report={'session':'Warp Verification','source':sources[region.get('source-0')],'settings':settings,'frames':len(actual),'maxError':float(delta.max()),'peak':float(np.abs(actual).max()),'directWorker':json.loads(run.stdout)}
Path('artifacts').mkdir(exist_ok=True)
Path('artifacts/'+settings['mode']+'-audio-validation.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report))
