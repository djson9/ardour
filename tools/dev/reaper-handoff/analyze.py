import csv,json,statistics,sys,bisect
from pathlib import Path
p=Path(sys.argv[1]) if len(sys.argv)>1 else Path(__file__).resolve().parent
events=list(csv.DictReader((p/'events.csv').open()))
traces=[[{k:float(v) for k,v in r.items()} for r in csv.DictReader((p/f'trace-{s}.csv').open())] for s in (0,1)]
start=float(events[0]['clock']);arm=float(next(e['clock'] for e in events if e['event']=='arm_before'))
on=float(next(e['clock'] for e in events if e['event']=='monitor_1.0'))
summary={'host':(p/'complete.txt').read_text().splitlines()[0], 'monitor_activation_ms':1000*(on-arm),'positions_discontinuous':[], 'state_counter_discontinuous':[], 'arm_processing_gaps_ms':[], 'render_lead_ms':{}}
for slot,rows in enumerate(traces):
 for a,b in zip(rows,rows[1:]):
  if a['play_state']!=1 or b['play_state']!=1:continue
  delta=b['position']-a['position']-a['frames']/a['sample_rate']
  if abs(delta)>1e-6:summary['positions_discontinuous'].append([slot,b['position'],delta])
  if b['samples_seen']!=a['samples_seen']+a['frames']:summary['state_counter_discontinuous'].append([slot,b['position']])
  if slot==0 and arm-.1<a['clock']<arm+.1 and b['clock']-a['clock']>.01:summary['arm_processing_gaps_ms'].append(1000*(b['clock']-a['clock']))
clocks=[r['clock'] for r in traces[1]]
for lo,hi,label in [(.5,1.5,'before_arm'),(2.7,3.5,'monitored'),(4.7,5.5,'after_disarm')]:
 diffs=[]
 for r in traces[0]:
  if r['play_state']!=1 or not lo<r['clock']-start<hi:continue
  index=bisect.bisect_left(clocks,r['clock']);candidates=traces[1][max(0,index-1):index+1]
  near=min(candidates,key=lambda x:abs(x['clock']-r['clock']))
  diffs.append(1000*(r['position']-near['position']))
 summary['render_lead_ms'][label]=statistics.median(diffs)
summary['playing_block_sizes']=sorted(set(r['frames'] for rows in traces for r in rows if r['play_state']==1))
assert not summary['positions_discontinuous'] and not summary['state_counter_discontinuous']
assert abs(summary['render_lead_ms']['monitored'])<5
assert summary['render_lead_ms']['before_arm']>20
(p/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
print(json.dumps(summary,indent=2))
