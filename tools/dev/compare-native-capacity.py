#!/usr/bin/env python3
"""Require exact, finite output from paired native ahead/direct capacity runs."""
import array,hashlib,json,math,pathlib,sys
assert len(sys.argv)==3,'Supply ahead and direct result.json paths'
results=[json.loads(pathlib.Path(p).read_text()) for p in sys.argv[1:]]
assert [r['mode'] for r in results]==['anticipated','direct']
assert all(r.get('passed') and r.get('native_registered_routes') for r in results)
for key in ('source_commit','library_sha256','block','chains','inserts_per_chain','verified_blocks'):
 assert results[0][key]==results[1][key],key
raw=[pathlib.Path(r['capture']).read_bytes() for r in results]
values=[]
for data in raw:
 v=array.array('f');v.frombytes(data);values.append(v)
finite=all(math.isfinite(x) for v in values for x in v)
diffs=[(i,abs(a-b)) for i,(a,b) in enumerate(zip(*values)) if a!=b]
identical=raw[0]==raw[1] and finite
result=dict(passed=identical,identical=identical,finite=finite,capture_bytes=[len(x) for x in raw],capture_sha256=[hashlib.sha256(x).hexdigest() for x in raw],different_samples=len(diffs),first_different_sample=diffs[0][0] if diffs else None,max_sample_error=max((v for _,v in diffs),default=0),mean_callback_ratio=results[1]['engine_mean_us']/results[0]['engine_mean_us'],source_commit=results[0]['source_commit'],hardware_deadlines_proven=False)
print(json.dumps(result,indent=2));sys.exit(0 if identical else 1)
