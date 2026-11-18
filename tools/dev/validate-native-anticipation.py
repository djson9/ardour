#!/usr/bin/env python3
"""Run all 13 native regressions within nine seconds; prepare binaries separately."""
import argparse, concurrent.futures, hashlib, json, os, pathlib, shlex, signal, subprocess, sys, time
started=time.monotonic()
root=pathlib.Path(__file__).resolve().parents[2]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('build',type=pathlib.Path)
parser.add_argument('--prepare',action='store_true',help='Compile the seven probe binaries; not part of test execution')
args=parser.parse_args();build=args.build.resolve()
cases=[('queue-contracts',{},'anticipation_queue_contract_probe.cc'),('plan-eligibility',{},'anticipation_plan_probe.cc')]
for block in (64,128):
 common=dict(BLOCK=block,AUTOMATIC=1,DISK_READER=1,SEND=1,SIDECHAIN=1,LOOP=1,PDC=1)
 cases += [(f'full-routing-{block}',common,'anticipation_native_route_probe.cc'),
           (f'gain-automation-{block}',dict(common,GAIN_AUTOMATION=1,NO_HANDOFF=1),'anticipation_native_route_probe.cc'),
           (f'monitor-{block}',dict(BLOCK=block),'anticipation_native_monitor_probe.cc'),
           (f'midi-{block}',dict(BLOCK=block),'anticipation_native_midi_probe.cc'),
           (f'recording-{block}',dict(BLOCK=block),'anticipation_native_record_probe.cc')]
cases += [('transport-controls-edits',dict(AUTOMATIC=1),'anticipation_native_transport_probe.cc')]
probes=sorted({probe for _,_,probe in cases})
if args.prepare:
 def prepare(probe):
  out=root/'artifacts'/probe.replace('.cc','').replace('_','-');out.mkdir(parents=True,exist_ok=True)
  with (out/'prepare.log').open('w') as log:
   result=subprocess.run([sys.executable,str(root/'tools/dev/run-anticipation-probe.py'),str(build),probe,'--build-only'],stdout=log,stderr=subprocess.STDOUT)
  print(json.dumps(dict(probe=probe,prepared=result.returncode==0,log=str(out/'prepare.log'))),flush=True)
  return result.returncode==0
 with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:passed=all(list(pool.map(prepare,probes)))
 raise SystemExit(0 if passed else 1)
# A stale binary fails promptly instead of silently spending minutes rebuilding.
deadline=started+9.0
dest=root/'artifacts'/('native-anticipation-validation-'+str(time.time_ns()));dest.mkdir(parents=True)
cache={}
def digest(path):
 p=pathlib.Path(path)
 if p not in cache:cache[p]=hashlib.sha256(p.read_bytes()).hexdigest() if p.exists() else None
 return cache[p]
try:
 metadata={}
 for probe in probes:
  out=root/'artifacts'/probe.replace('.cc','').replace('_','-')
  record=json.loads((out/'prepared.json').read_text())
  assert record['build_root']==str(build),'build directory changed'
  for path,expected in record['inputs'].items():assert expected is not None and digest(path)==expected,'changed input: '+path
  assert digest(out/'probe')==record['probe_sha256'],'changed probe: '+probe
  assert digest(out/'private-home.dylib')==record['isolation_sha256'],'changed isolation helper'
  metadata[probe]=record
 # Library source changes require a matching rebuilt library, as in the original runner.
 for checkout in {root,build}:
  assert subprocess.run(['git','diff','--quiet',metadata[probes[0]]['linked_source_commit'],'--','libs'],cwd=checkout,timeout=max(.01,deadline-time.monotonic())).returncode==0,'library sources changed'
except (OSError,AssertionError,ValueError,subprocess.TimeoutExpired) as error:
 print('Preparation required: '+str(error)+'\nRun: python3 tools/dev/validate-native-anticipation.py '+shlex.quote(str(build))+' --prepare',file=sys.stderr)
 raise SystemExit(1)
pref=pathlib.Path.home()/'Library/Preferences/Ardour9/config';before=digest(pref)
environment={k:v for k,v in os.environ.items() if not k.startswith('ARDOUR_PROBE_') and k not in ('BASH_ENV','ENV')}
jobs=[]
try:
 for name,flags,probe in cases:
  if time.monotonic()>=deadline:raise TimeoutError('nine-second suite deadline exhausted during setup')
  out=root/'artifacts'/probe.replace('.cc','').replace('_','-');case=dest/name;case.mkdir()
  private=case/'private-home';private.mkdir()
  env={**environment,'LV2_PATH':str(build/'build/libs/LV2'),'ARDOUR_PROBE_HOME':str(private),'ARDOUR_PROBE_RT_THREADS':'1','ARDOUR_PROBE_PRIVATE_SEMAPHORES':'1',**{'ARDOUR_PROBE_'+key:str(value) for key,value in flags.items()}}
  script='TOP='+shlex.quote(str(build))+'\n. "$TOP/build/gtk2_ardour/ardev_common_waf.sh"\nexport DYLD_INSERT_LIBRARIES='+shlex.quote(str(out/'private-home.dylib'))+'\nexec '+shlex.join([str(out/'probe'),str(case/'session')])
  log=(case/'run.log').open('w')
  process=subprocess.Popen(['/bin/bash','-c',script],env=env,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
  jobs.append((name,probe,case,process,log,time.monotonic()))
 while any(process.poll() is None for _,_,_,process,_,_ in jobs) and time.monotonic()<deadline:time.sleep(.01)
finally:
 for _,_,_,process,log,_ in jobs:
  if process.poll() is None:
   try:os.killpg(process.pid,signal.SIGKILL)
   except ProcessLookupError:pass
  process.wait();log.close()
results=[]
for name,probe,case,process,_,began in jobs:
 output=(case/'run.log').read_text();result={}
 for line in output.splitlines():
  if line.startswith('RESULT '):result=json.loads(line[7:])
 item=dict(case=name,exit_code=process.returncode,passed=process.returncode==0 and result.get('passed',False),result=result,probe_sha256=metadata[probe]['probe_sha256'])
 (case/'result.json').write_text(json.dumps(item,indent=2)+'\n');results.append(item)
 print(json.dumps({k:item[k] for k in ('case','passed','exit_code')}),flush=True)
cache.pop(pref,None);unchanged=digest(pref)==before
library=build/'build/libs/ardour/libardour.dylib';library_before=digest(library);cache.pop(library,None);library_unchanged=digest(library)==library_before
elapsed=time.monotonic()-started
summary=dict(passed=len(results)==len(cases) and all(i['passed'] for i in results) and unchanged and library_unchanged and elapsed<10,elapsed_seconds=elapsed,execution_deadline_seconds=9,under_ten_seconds=elapsed<10,user_config_unchanged=unchanged,library_unchanged=library_unchanged,validation_runner_sha256=digest(pathlib.Path(__file__)),hardware_deadlines_proven=False,source_commit=metadata[probes[0]]['source_commit'],library_sha256=digest(build/'build/libs/ardour/libardour.dylib'),cases=results)
(dest/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
(dest/'prepared-provenance.json').write_text(json.dumps(metadata,indent=2)+'\n')
print(json.dumps({k:v for k,v in summary.items() if k!='cases'}),flush=True);print('EVIDENCE '+str(dest),flush=True)
raise SystemExit(0 if summary['passed'] else 1)
