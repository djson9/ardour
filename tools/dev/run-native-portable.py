#!/usr/bin/env python3
"""Run only the isolated native benchmark in the local GUI domain."""
import hashlib,json,os,pathlib,plistlib,shlex,subprocess,sys,time
bundle=pathlib.Path(sys.argv[1]).resolve(); mode=sys.argv[2] if len(sys.argv)>2 else 'ahead'
assert mode in ('ahead','direct')
soak='--soak' in sys.argv[3:]
command_started=time.monotonic()
out=bundle.parent/('native-coreaudio-'+mode+'-'+str(time.time_ns()));out.mkdir()
private=out/'private-home';private.mkdir();empty=out/'empty';empty.mkdir()
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest() if p.exists() else None
pref=pathlib.Path.home()/'Library/Preferences/Ardour9/config';before=digest(pref)
manifest=json.loads((bundle/'manifest.json').read_text())
for entry in manifest['files']:assert digest(bundle/entry['path'])==entry['relocated_sha256'],entry['path']
env={k:v for k,v in os.environ.items() if k.startswith('ARDOUR_PROBE_')}
env.update(ARDOUR_PROBE_HOME=str(private),DYLD_INSERT_LIBRARIES=str(bundle/'private-home.dylib'),ARDOUR_DLL_PATH=str(bundle/'lib'),ARDOUR_CONFIG_PATH=str(empty),ARDOUR_DATA_PATH=str(bundle/'share'),ARDOUR_BACKEND_PATH=str(bundle/'backends'),ARDOUR_PANNER_PATH=str(bundle/'panners'),ARDOUR_SURFACES_PATH=str(empty),VAMP_PATH=str(empty),LV2_PATH=str(bundle/'LV2'),ARDOUR_PROBE_BACKEND=os.environ.get('ARDOUR_PROBE_BACKEND','CoreAudio'),ARDOUR_PROBE_OUTPUT_DEVICE=os.environ.get('ARDOUR_PROBE_OUTPUT_DEVICE','BlackHole 64ch'),ARDOUR_PROBE_BLOCK=os.environ.get('ARDOUR_PROBE_BLOCK','64'))
if (bundle/'native-probe-activity.dylib').exists():env['DYLD_INSERT_LIBRARIES']+=':'+str(bundle/'native-probe-activity.dylib')
if mode=='direct':env['ARDOUR_PROBE_DIRECT']='1'
env.setdefault('ARDOUR_PROBE_BLOCKS','512')
if not soak and int(env['ARDOUR_PROBE_BLOCKS'])*int(env['ARDOUR_PROBE_BLOCK'])/48000>2:
 raise SystemExit('Capture longer than two seconds requires explicit --soak; default command is bounded below ten seconds')
status=out/'exit-code';label='dev.ardour.native-capacity.'+str(os.getpid());domain='gui/'+str(os.getuid())
script='export DYLD_INSERT_LIBRARIES='+shlex.quote(env['DYLD_INSERT_LIBRARIES'])+'\n'+shlex.join([str(bundle/'probe'),str(out/'session')])+'\nrc=$?\nprintf "%s" "$rc" > '+shlex.quote(str(status))+'\n'
job=dict(Label=label,ProgramArguments=['/bin/bash','-c',script],EnvironmentVariables=env,RunAtLoad=True,StandardOutPath=str(out/'run.log'),StandardErrorPath=str(out/'run.log'))
plist=out/'probe.plist';plist.write_bytes(plistlib.dumps(job));started=time.monotonic();timeout=(float(os.environ.get('ARDOUR_PROBE_TIMEOUT',90+int(env['ARDOUR_PROBE_BLOCKS'])*int(env['ARDOUR_PROBE_BLOCK'])/48000)) if soak else max(.01,8.5-(time.monotonic()-command_started)))
if os.environ.get('ARDOUR_PROBE_CONSOLE'):
 with (out/'run.log').open('w') as log:
  process=subprocess.Popen([str(bundle/'probe'),str(out/'session')],env={**os.environ,**env},stdout=log,stderr=subprocess.STDOUT)
  try:code=process.wait(timeout=timeout)
  except subprocess.TimeoutExpired:
   process.terminate()
   try:process.wait(timeout=.2 if not soak else 5)
   except subprocess.TimeoutExpired:process.kill();process.wait()
   code='timeout'
else:
 subprocess.run(['launchctl','bootstrap',domain,str(plist)],check=True)
 try:
  deadline=started+timeout
  while not status.exists() and time.monotonic()<deadline:time.sleep(.2)
  code=int(status.read_text()) if status.exists() else 'timeout'
 finally:subprocess.run(['launchctl','bootout',domain+'/'+label],check=False,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
output=(out/'run.log').read_text();assert digest(pref)==before,'User config changed'
result=dict(exit_status=code,user_config_unchanged=True,runtime_hashes_verified=True,source_commit=manifest['source_commit'],source_patch_sha256=manifest.get('source_patch_sha256'),working_tree_status=manifest.get('working_tree_status'),runtime_manifest_sha256=digest(bundle/'manifest.json'),elapsed_seconds=time.monotonic()-started,settings=env,explicit_soak=soak,physical_loopback_latency_measured=False,latency_critical_activity='latency_critical_activity=active' in output,capture_memory_locked='CAPTURE_MEMORY locked_bytes=' in output)
for line in output.splitlines():
 if line.startswith('RESULT '):result['probe_result']=json.loads(line[7:])
probe=result.get('probe_result',{})
if probe.get('capture'):result['capture_sha256']=digest(pathlib.Path(probe['capture']))
if probe.get('block'):result['measured_callback_budget_met']=probe['observed_engine_max_us']<=1e6*probe['block']/48000 and probe['playback_xruns']==0 and probe['cache_underruns']==0
result['command_elapsed_seconds']=time.monotonic()-command_started
result['under_ten_seconds']=result['command_elapsed_seconds']<10
(out/'result.json').write_text(json.dumps(result,indent=2));print(output);print('EVIDENCE '+str(out),flush=True)
sys.exit(0 if code==0 and probe.get('passed') and (soak or result['under_ten_seconds']) else 1)
