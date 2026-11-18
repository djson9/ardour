#!/usr/bin/env python3
"""Native engine integration regression; retains logs and exact linked provenance."""
import json, os, pathlib, shutil, subprocess, sys, time
root=pathlib.Path(__file__).resolve().parents[2]
build=pathlib.Path(sys.argv[1]).resolve()
dest=root/'artifacts'/('native-anticipation-validation-'+str(time.time_ns()));dest.mkdir(parents=True)
cases=[('queue-contracts',{},'anticipation_queue_contract_probe.cc'),('plan-eligibility',{},'anticipation_plan_probe.cc')]
for block in (64,128):
 common=dict(BLOCK=block,AUTOMATIC=1,DISK_READER=1,SEND=1,SIDECHAIN=1,LOOP=1,PDC=1)
 cases += [(f'full-routing-{block}',common,'anticipation_native_route_probe.cc'),
           (f'gain-automation-{block}',dict(common,GAIN_AUTOMATION=1,NO_HANDOFF=1),'anticipation_native_route_probe.cc'),
           (f'monitor-{block}',dict(BLOCK=block),'anticipation_native_monitor_probe.cc'),
           (f'midi-{block}',dict(BLOCK=block),'anticipation_native_midi_probe.cc'),
           (f'recording-{block}',dict(BLOCK=block),'anticipation_native_record_probe.cc')]
cases += [('transport-controls-edits',dict(AUTOMATIC=1),'anticipation_native_transport_probe.cc')]
results=[]
for name,flags,probe in cases:
 env={k:v for k,v in os.environ.items() if not k.startswith('ARDOUR_PROBE_')};env.update({'ARDOUR_PROBE_'+k:str(v) for k,v in flags.items()})
 with (dest/(name+'.log')).open('w') as log:
  result=subprocess.run([sys.executable,str(root/'tools/dev/run-anticipation-probe.py'),str(build),probe],env=env,stdout=log,stderr=subprocess.STDOUT,timeout=150)
 item=dict(case=name,exit_code=result.returncode,passed=False)
 source=root/'artifacts'/probe.replace('.cc','').replace('_','-')/'result.json'
 if source.exists():
  shutil.copyfile(source,dest/(name+'.json'));data=json.loads(source.read_text());item['passed']=result.returncode==0 and data.get('passed',False)
 results.append(item);print(json.dumps(item),flush=True)
summary=dict(passed=all(i['passed'] for i in results),cases=results)
(dest/'summary.json').write_text(json.dumps(summary,indent=2));print('EVIDENCE '+str(dest),flush=True)
sys.exit(0 if summary['passed'] else 1)
