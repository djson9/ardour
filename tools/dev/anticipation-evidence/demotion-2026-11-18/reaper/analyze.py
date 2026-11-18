import csv,json,bisect,statistics,sys,gzip
from pathlib import Path
p=Path(sys.argv[1]);events=list(csv.DictReader((p/'events.csv').open()));t=float(next(e['clock'] for e in events if e['event']=='disarm_before'))
traces=[[{k:float(v) for k,v in row.items()} for row in csv.DictReader(gzip.open(p/f'trace-{i}.csv.gz','rt') if (p/f'trace-{i}.csv.gz').exists() else (p/f'trace-{i}.csv').open())] for i in (0,1)]
fault={k:float(v) for k,v in next(csv.DictReader((p/'fault.csv').open())).items()}
r={'host':(p/'complete.txt').read_text().splitlines()[0],'fault':fault,'actual_stall_ms':1000*(fault['end']-fault['start']),'nominal_block_frames':sorted({x['frames'] for rows in traces for x in rows if x['play_state']==1}),'max_gap_near_disarm_ms':[],'timeline_discontinuities':[],'state_discontinuities':[],'physical_audio_measured':False}
for i,rows in enumerate(traces):
 pairs=[(a,b) for a,b in zip(rows,rows[1:]) if a['play_state']==b['play_state']==1]
 r['max_gap_near_disarm_ms'].append(max(1000*(b['clock']-a['clock']) for a,b in pairs if t-.05<a['clock']<t+.15))
 r['timeline_discontinuities'].append(sum(abs(b['position']-a['position']-a['frames']/a['sample_rate'])>1e-6 for a,b in pairs))
 r['state_discontinuities'].append(sum(b['samples_seen']!=a['samples_seen']+a['frames'] for a,b in pairs))
clocks=[x['clock'] for x in traces[1]];leads=[]
for row in traces[0]:
 if not (t+.5<row['clock']<t+1.1):continue
 idx=bisect.bisect_left(clocks,row['clock']);near=min(traces[1][max(0,idx-1):idx+1],key=lambda x:abs(x['clock']-row['clock']))
 leads.append(1000*(row['position']-near['position']))
r['restored_ahead_lead_ms']=statistics.median(leads)
assert not any(r['timeline_discontinuities']) and not any(r['state_discontinuities'])
assert r['restored_ahead_lead_ms']>150
(p/'summary.json').write_text(json.dumps(r,indent=2)+'\n');print(p.name,json.dumps(r))
