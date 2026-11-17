#!/usr/bin/env python3
"""Build a standalone probe against an explicitly selected existing Ardour build.
Does not rebuild, install, start or stop the user's GUI application.
"""
import ast, hashlib, json, os, pathlib, shlex, subprocess, sys, tempfile, plistlib, time, re
root=pathlib.Path(__file__).resolve().parents[2]
base=pathlib.Path(sys.argv[1]).resolve()
source=sys.argv[2] if len(sys.argv)>2 else 'anticipation_probe.cc'
assert source in ('anticipation_probe.cc','anticipation_callback_probe.cc','anticipation_capacity_probe.cc','anticipation_graph_probe.cc','anticipation_eligibility_probe.cc','anticipation_pdc_probe.cc','anticipation_seek_probe.cc','anticipation_record_probe.cc','anticipation_loop_probe.cc','anticipation_midi_boundary_probe.cc')
out=root/'artifacts'/source.replace('.cc','').replace('_','-');out.mkdir(parents=True,exist_ok=True)
(out/'result.json').unlink(missing_ok=True)
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest() if p.exists() else None
library_path=base/'build/libs/ardour/libardour.dylib'
library_before=digest(library_path)
def run(args,**kw):return subprocess.check_output(args,text=True,**kw)
head=run(['git','rev-parse','HEAD'],cwd=root).strip()
base_head=run(['git','rev-parse','HEAD'],cwd=base).strip()
for checkout in [root,base]:
 if subprocess.run(['git','diff','--quiet',base_head,'--','libs'],cwd=checkout).returncode:
  raise SystemExit('Library sources differ from selected build revision')
cache={}
for line in (base/'build/c4che/_cache.py').read_text().splitlines():
 if ' = ' in line:
  k,v=line.split(' = ',1)
  try:cache[k]=ast.literal_eval(v)
  except (ValueError,SyntaxError):pass
flags=list(cache['CXXFLAGS'])
incs=[root,base/'build',root/'libs',root/'libs/ardour/test',root/'libs/libltc/ltc']
incs+=list((root/'libs').iterdir())+list((root/'libs/tk').iterdir())
incs+=list((base/'build/libs').iterdir())
for k,v in cache.items():
 if k.startswith('INCLUDES') and isinstance(v,list):incs.extend(map(pathlib.Path,v))
cmd=['/usr/bin/clang++',*flags,'-std=c++17','-Wno-deprecated-declarations','-Wno-unused-function','-Wno-inconsistent-missing-override']
cmd += ['-I'+str(p) for p in incs if p.is_dir()]
cmd += [str(root/'tools/dev'/source),str(base/'build/libs/ardour/test/test_ui.cc.4.o'),str(base/'build/libs/ardour/test/dummy_lxvst.cc.4.o')]
deps=[]
for line in run(['otool','-L',str(base/'build/libs/ardour/run-tests')]).splitlines()[1:]:
 p=line.strip().split(' (')[0]
 if p.startswith(('/opt/homebrew/',str(base))) and p.endswith('.dylib'):deps.append(p)
cmd+=deps+['-framework','CoreFoundation','-framework','AppKit','-o',str(out/'probe')]
(out/'compile-command.json').write_text(json.dumps(cmd,indent=2))
built=subprocess.run(cmd)
if built.returncode:raise SystemExit(built.returncode)
if digest(library_path)!=library_before:raise SystemExit('Library changed while compiling; finish its build, then rerun')
pref=pathlib.Path.home()/'Library/Preferences/Ardour9/config'
before=digest(pref)
session=str(pathlib.Path(tempfile.mkdtemp(prefix='run-',dir=out))/'session')
# Interpose GLib's home lookup only inside our executable, leaving HOME and
# the user's application untouched. Ardour uses this lookup for Mac preferences.
isolation=out/'private-home.c'
isolation.write_text('#include <stdlib.h>\nextern const char *g_get_home_dir(void);\nstatic const char *private_home(void){ const char *p=getenv("ARDOUR_PROBE_HOME"); if(!p) abort(); return p; }\n__attribute__((used,section("__DATA,__interpose"))) static const struct { const void *replacement; const void *original; } hook={(void*)private_home,(void*)g_get_home_dir};\n')
subprocess.run(['/usr/bin/clang','-dynamiclib',str(isolation),'/opt/homebrew/opt/glib/lib/libglib-2.0.dylib','-o',str(out/'private-home.dylib')],check=True)
env=os.environ.copy()
env['LV2_PATH']=str(base/'build/libs/LV2')
env['ARDOUR_PROBE_HOME']=str(out/'private-home');pathlib.Path(env['ARDOUR_PROBE_HOME']).mkdir(exist_ok=True)
env['DYLD_INSERT_LIBRARIES']=str(out/'private-home.dylib')
script='TOP='+shlex.quote(str(base))+'\n. "$TOP/build/gtk2_ardour/ardev_common_waf.sh"\nexport DYLD_INSERT_LIBRARIES='+shlex.quote(str(out/'private-home.dylib'))+'\nexec '+shlex.join([str(out/'probe'),session])
if env.get('ARDOUR_PROBE_GUI')=='1':
 # Run the same isolated test in the existing Aqua session when testing CoreAudio.
 # The transient launchd job is always removed; no persistent service is installed.
 label='dev.ardour.anticipation-probe.'+str(os.getpid())
 domain='gui/'+str(os.getuid())
 status=pathlib.Path(session).parent/'exit-code';plist=pathlib.Path(session).parent/'probe.plist'
 gui_script=script.replace('\nexec ','\n')+'\nrc=$?\nprintf "%s" "$rc" > '+shlex.quote(str(status))+'\n'
 job={'Label':label,'ProgramArguments':['/bin/bash','-c',gui_script],'RunAtLoad':True,
      'EnvironmentVariables':{k:v for k,v in env.items() if k.startswith('ARDOUR_PROBE_') or k=='LV2_PATH'},
      'StandardOutPath':str(out/'run.log'),'StandardErrorPath':str(out/'run.log')}
 (out/'run.log').write_text('');plist.write_bytes(plistlib.dumps(job))
 subprocess.run(['launchctl','bootstrap',domain,str(plist)],check=True)
 try:
  deadline=time.monotonic()+90
  while not status.exists() and time.monotonic()<deadline:time.sleep(.2)
  if not status.exists():raise RuntimeError('GUI probe timeout; see run.log')
  code=int(status.read_text())
 finally:subprocess.run(['launchctl','bootout',domain+'/'+label],check=False)
 output=(out/'run.log').read_text()
else:
 with (out/'run.log').open('w') as log:
  p=subprocess.run(['/bin/bash','-c',script],env=env,text=True,stdout=log,stderr=subprocess.STDOUT,timeout=90)
 code=p.returncode;output=(out/'run.log').read_text()
print(output)
if digest(pref)!=before:raise SystemExit('User config changed during probe')
if digest(library_path)!=library_before:raise SystemExit('Library changed during probe; result discarded')
if code:raise SystemExit(code)
result=json.loads(next(x[7:] for x in output.splitlines() if x.startswith('RESULT ')))
if result.get('normal_session_callback') and 'blocks' in result:
 result['correctness_passed']=result['passed']
 period=1e6*result['blocks'][0]/48000
 observed=[float(v) for v in re.findall(r'ENGINE (?:observed|steady)_max_us=([\d.]+)',output)]
 result['nominal_callback_period_us']=period
 result['observed_engine_max_us']=max(observed+[result['engine_max_us']])
 result['measured_callback_budget_met']=result['observed_engine_max_us']<=period and result['bridge_deadline_overruns']==0
 result['timing_limitation']='Dummy backend pacing does not establish hardware deadlines or input/output latency.'
 result['routing_and_input_evidence']=[line for line in output.splitlines() if line.startswith(('PLAYLIST ','MIDCHAIN TAP ','BUS INPUT ERROR ','MIDI ','DISK_READER ','DISK_REFILL ','READER INPUT ','READER PREFILL ','CACHED GRAPH ','SIDECHAIN ','MAIN OUTPUT ','PDC ','SEEK '))]
for line in output.splitlines():
 if line.startswith('REAL_PLUGIN_RESULT '):result['real_plugins']=json.loads(line[len('REAL_PLUGIN_RESULT '):])
result.update(source_commit=head,linked_source_commit=base_head,library_sha256=digest(base/'build/libs/ardour/libardour.dylib'),user_config_unchanged=True)
result['source_sha256']={p.name:digest(p) for p in [*sorted((root/'tools/dev').glob('anticipation*probe.cc')),pathlib.Path(__file__).resolve()]}
result['probe_settings']={k:v for k,v in env.items() if k.startswith('ARDOUR_PROBE_')}
result['delay_plugin_sha256']=digest(base/'build/libs/LV2/a-delay.lv2/a-delay.dylib')
result['synth_plugin_sha256']=digest(base/'build/libs/LV2/reasonablesynth.lv2/reasonablesynth.dylib')
result['compressor_plugin_sha256']=digest(base/'build/libs/LV2/a-comp.lv2/a-comp.dylib')
result['working_tree_status']=run(['git','status','--short'],cwd=root).splitlines()
(out/'result.json').write_text(json.dumps(result,indent=2))
