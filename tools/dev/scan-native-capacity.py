"""Inspect the full retained stereo float capture after a timed run has ended."""
import hashlib,json,pathlib,sys
import numpy as np
p=pathlib.Path(sys.argv[1]);document=json.loads(p.read_text());r=document['probe_result'];capture=pathlib.Path(r['capture'])
x=np.memmap(capture,dtype='<f4',mode='r');assert x.size==2*r['block']*r['verified_blocks']
rms=[];finite=True;minimum=float('inf');maximum=float('-inf')
for start in range(0,x.size,96000):
 chunk=np.asarray(x[start:start+96000],dtype=np.float64)
 finite=finite and bool(np.isfinite(chunk).all())
 rms.append(float(np.sqrt(np.mean(chunk*chunk))))
 minimum=min(minimum,float(chunk.min()));maximum=max(maximum,float(chunk.max()))
h=hashlib.sha256()
with capture.open('rb') as f:
 for data in iter(lambda:f.read(1024*1024),b''):h.update(data)
assert h.hexdigest()==document['capture_sha256']
result=dict(capture_sha256=h.hexdigest(),frames=x.size//2,finite=finite,seconds_or_final_partial_second=len(rms),minimum_one_second_rms=min(rms),maximum_one_second_rms=max(rms),minimum_sample=minimum,maximum_sample=maximum,unexpected_silent_seconds=sum(v<1e-6 for v in rms),performed_after_timed_run=True,physical_delivery_verified=False,within_unity=minimum>=-1 and maximum<=1)
print(json.dumps(result,indent=2));sys.exit(0 if finite and min(rms)>1e-6 and minimum>=-1 and maximum<=1 else 1)
