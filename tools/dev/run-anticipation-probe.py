#!/usr/bin/env python3
"""Build a standalone probe against an explicitly selected existing Ardour build.
Does not rebuild, install, start or stop the user's GUI application.
"""
import ast, hashlib, json, os, pathlib, shlex, subprocess, sys, tempfile
root=pathlib.Path(__file__).resolve().parents[2]
base=pathlib.Path(sys.argv[1]).resolve()
out=root/'artifacts'/'anticipation-probe';out.mkdir(parents=True,exist_ok=True)
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
cmd=['/usr/bin/clang++',*flags,'-std=c++17','-Wno-deprecated-declarations']
cmd += ['-I'+str(p) for p in incs if p.is_dir()]
cmd += [str(root/'tools/dev/anticipation_probe.cc'),str(base/'build/libs/ardour/test/test_ui.cc.4.o'),str(base/'build/libs/ardour/test/dummy_lxvst.cc.4.o')]
deps=[]
for line in run(['otool','-L',str(base/'build/libs/ardour/run-tests')]).splitlines()[1:]:
 p=line.strip().split(' (')[0]
 if p.startswith(('/opt/homebrew/',str(base))) and p.endswith('.dylib'):deps.append(p)
cmd+=deps+['-framework','CoreFoundation','-framework','AppKit','-o',str(out/'probe')]
(out/'compile-command.json').write_text(json.dumps(cmd,indent=2))
built=subprocess.run(cmd)
if built.returncode:raise SystemExit(built.returncode)
pref=pathlib.Path.home()/'Library/Preferences/Ardour9/config'
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest() if p.exists() else None
before=digest(pref)
session=str(pathlib.Path(tempfile.mkdtemp(prefix='run-',dir=out))/'session')
# Interpose GLib's home lookup only inside our executable, leaving HOME and
# the user's application untouched. Ardour uses this lookup for Mac preferences.
isolation=out/'private-home.c'
isolation.write_text('#include <stdlib.h>\nextern const char *g_get_home_dir(void);\nstatic const char *private_home(void){ const char *p=getenv("ARDOUR_PROBE_HOME"); if(!p) abort(); return p; }\n__attribute__((used,section("__DATA,__interpose"))) static const struct { const void *replacement; const void *original; } hook={(void*)private_home,(void*)g_get_home_dir};\n')
subprocess.run(['/usr/bin/clang','-dynamiclib',str(isolation),'/opt/homebrew/opt/glib/lib/libglib-2.0.dylib','-o',str(out/'private-home.dylib')],check=True)
env=os.environ.copy()
env['ARDOUR_PROBE_HOME']=str(out/'private-home');pathlib.Path(env['ARDOUR_PROBE_HOME']).mkdir(exist_ok=True)
env['DYLD_INSERT_LIBRARIES']=str(out/'private-home.dylib')
script='TOP='+shlex.quote(str(base))+'\n. "$TOP/build/gtk2_ardour/ardev_common_waf.sh"\nexport DYLD_INSERT_LIBRARIES='+shlex.quote(str(out/'private-home.dylib'))+'\nexec '+shlex.join([str(out/'probe'),session])
p=subprocess.run(['/bin/bash','-c',script],env=env,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=90)
(out/'run.log').write_text(p.stdout);print(p.stdout)
if digest(pref)!=before:raise SystemExit('User config changed during probe')
if p.returncode:raise SystemExit(p.returncode)
result=json.loads(next(x[7:] for x in p.stdout.splitlines() if x.startswith('RESULT ')))
result.update(source_commit=head,linked_source_commit=base_head,library_sha256=digest(base/'build/libs/ardour/libardour.dylib'),user_config_unchanged=True)
(out/'result.json').write_text(json.dumps(result,indent=2))
