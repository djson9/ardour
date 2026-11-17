import csv,json,statistics,sys,bisect
from pathlib import Path
p=Path(sys.argv[1]) if len(sys.argv)>1 else Path(__file__).resolve().parent
events=list(csv.DictReader((p/'events.csv').open()))
traces=[[{k:float(v) for k,v in r.items()} for r in csv.DictReader((p/f'trace-{s}.csv').open())] for s in (0,1)]
action=(p/'action.txt').read_text().strip() if (p/'action.txt').exists() else 'monitor'
start=float(events[0]['clock']);arm=float(next(e['clock'] for e in events if e['event']==('arm_before' if action=='monitor' else 'seek_back_before')))
on=float(next((e['clock'] for e in events if e['event']=='monitor_1.0'),arm))
gap_label='arm_processing_gaps_ms' if action=='monitor' else 'first_seek_processing_gaps_ms'
summary={'host':(p/'complete.txt').read_text().splitlines()[0], 'action':action,'monitor_activation_ms':1000*(on-arm) if action=='monitor' else None,'positions_discontinuous':[], 'state_counter_discontinuous':[], gap_label:[], 'render_lead_ms':{}}
if action=='seek':summary['state_after_seek']=[]
for slot,rows in enumerate(traces):
 for a,b in zip(rows,rows[1:]):
  if a['play_state']!=1 or b['play_state']!=1:continue
  delta=b['position']-a['position']-a['frames']/a['sample_rate']
  if abs(delta)>1e-6:summary['positions_discontinuous'].append([slot,b['position'],delta])
  if b['samples_seen']!=a['samples_seen']+a['frames']:
   summary['state_counter_discontinuous'].append([slot,b['position']])
   if action=='seek':summary['state_after_seek'].append(dict(track=slot,position=b['position'],samples_seen=b['samples_seen'],history=b['history']))
  if slot==0 and arm-.1<a['clock']<arm+.1 and b['clock']-a['clock']>.01:summary[gap_label].append(1000*(b['clock']-a['clock']))
clocks=[r['clock'] for r in traces[1]]
labels=['before_arm','monitored','after_disarm'] if action=='monitor' else ['before_first_seek','between_seeks','after_second_seek']
for (lo,hi),label in zip([(.5,1.5),(2.7,3.5),(4.7,5.5)],labels):
 diffs=[]
 for r in traces[0]:
  if r['play_state']!=1 or not lo<r['clock']-start<hi:continue
  index=bisect.bisect_left(clocks,r['clock']);candidates=traces[1][max(0,index-1):index+1]
  near=min(candidates,key=lambda x:abs(x['clock']-r['clock']))
  diffs.append(1000*(r['position']-near['position']))
 summary['render_lead_ms'][label]=statistics.median(diffs)
summary['playing_block_sizes']=sorted(set(r['frames'] for rows in traces for r in rows if r['play_state']==1))
if action=='monitor':
 assert not summary['positions_discontinuous'] and not summary['state_counter_discontinuous']
 assert abs(summary['render_lead_ms']['monitored'])<5
else:
 assert len(summary['positions_discontinuous'])>=4
 assert len(summary['state_after_seek'])==4 and all(x['samples_seen']==0 and x['history']==0 for x in summary['state_after_seek'])
assert summary['render_lead_ms'][labels[0]]>20
(p/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
print(json.dumps(summary,indent=2))
