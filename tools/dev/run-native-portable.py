#!/usr/bin/env python3
"""Run only the isolated native benchmark in the local GUI domain."""
import hashlib,json,os,pathlib,plistlib,shlex,subprocess,sys,time
bundle=pathlib.Path(sys.argv[1]).resolve(); mode=sys.argv[2] if len(sys.argv)>2 else 'ahead'
assert mode in ('ahead','direct')
out=bundle.parent/('native-coreaudio-'+mode+'-'+str(time.time_ns()));out.mkdir()
private=out/'private-home';private.mkdir();empty=out/'empty';empty.mkdir()
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest() if p.exists() else None
pref=pathlib.Path.home()/'Library/Preferences/Ardour9/config';before=digest(pref)
manifest=json.loads((bundle/'manifest.json').read_text())
for entry in manifest['files']:assert digest(bundle/entry['path'])==entry['relocated_sha256'],entry['path']
env={k:v for k,v in os.environ.items() if k.startswith('ARDOUR_PROBE_')}
env.update(ARDOUR_PROBE_HOME=str(private),DYLD_INSERT_LIBRARIES=str(bundle/'private-home.dylib'),ARDOUR_DLL_PATH=str(bundle/'lib'),ARDOUR_CONFIG_PATH=str(empty),ARDOUR_DATA_PATH=str(bundle/'share'),ARDOUR_BACKEND_PATH=str(bundle/'backends'),ARDOUR_PANNER_PATH=str(bundle/'panners'),ARDOUR_SURFACES_PATH=str(empty),VAMP_PATH=str(empty),LV2_PATH=str(bundle/'LV2'),ARDOUR_PROBE_BACKEND=os.environ.get('ARDOUR_PROBE_BACKEND','CoreAudio'),ARDOUR_PROBE_OUTPUT_DEVICE=os.environ.get('ARDOUR_PROBE_OUTPUT_DEVICE','BlackHole 64ch'),ARDOUR_PROBE_BLOCK=os.environ.get('ARDOUR_PROBE_BLOCK','64'))
if mode=='direct':env['ARDOUR_PROBE_DIRECT']='1'
status=out/'exit-code';label='dev.ardour.native-capacity.'+str(os.getpid());domain='gui/'+str(os.getuid())
script='export DYLD_INSERT_LIBRARIES='+shlex.quote(str(bundle/'private-home.dylib'))+'\n'+shlex.join([str(bundle/'probe'),str(out/'session')])+'\nrc=$?\nprintf "%s" "$rc" > '+shlex.quote(str(status))+'\n'
job=dict(Label=label,ProgramArguments=['/bin/bash','-c',script],EnvironmentVariables=env,RunAtLoad=True,StandardOutPath=str(out/'run.log'),StandardErrorPath=str(out/'run.log'))
plist=out/'probe.plist';plist.write_bytes(plistlib.dumps(job));started=time.monotonic()
if os.environ.get('ARDOUR_PROBE_CONSOLE'):
 with (out/'run.log').open('w') as log:
  process=subprocess.Popen([str(bundle/'probe'),str(out/'session')],env={**os.environ,**env},stdout=log,stderr=subprocess.STDOUT)
  try:code=process.wait(timeout=90)
  except subprocess.TimeoutExpired:
   process.terminate()
   try:process.wait(timeout=5)
   except subprocess.TimeoutExpired:process.kill();process.wait()
   code='timeout'
else:
 subprocess.run(['launchctl','bootstrap',domain,str(plist)],check=True)
 try:
  deadline=started+90
  while not status.exists() and time.monotonic()<deadline:time.sleep(.2)
  code=int(status.read_text()) if status.exists() else 'timeout'
 finally:subprocess.run(['launchctl','bootout',domain+'/'+label],check=False,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
output=(out/'run.log').read_text();assert digest(pref)==before,'User config changed'
result=dict(exit_status=code,user_config_unchanged=True,runtime_hashes_verified=True,source_commit=manifest['source_commit'],runtime_manifest_sha256=digest(bundle/'manifest.json'),elapsed_seconds=time.monotonic()-started,settings=env,physical_loopback_latency_measured=False)
for line in output.splitlines():
 if line.startswith('RESULT '):result['probe_result']=json.loads(line[7:])
probe=result.get('probe_result',{})
if probe.get('capture'):result['capture_sha256']=digest(pathlib.Path(probe['capture']))
if probe.get('block'):result['measured_callback_budget_met']=probe['observed_engine_max_us']<=1e6*probe['block']/48000 and probe['playback_xruns']==0 and probe['cache_underruns']==0
(out/'result.json').write_text(json.dumps(result,indent=2));print(output);print('EVIDENCE '+str(out),flush=True)
sys.exit(0 if code==0 and probe.get('passed') else 1)
