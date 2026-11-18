import ast,pathlib,subprocess,sys
r=pathlib.Path(__file__).resolve().parents[2];cache={}
for line in (r/'build/c4che/_cache.py').read_text().splitlines():
 if ' = ' in line:
  k,v=line.split(' = ',1)
  try:cache[k]=ast.literal_eval(v)
  except (ValueError,SyntaxError):pass
incs=list((r/'libs/tk').iterdir())+list((r/'build/libs/tk').iterdir())
for k,v in cache.items():
 if k.startswith('INCLUDES') and isinstance(v,list):incs.extend(map(pathlib.Path,v))
deps=[]
for line in subprocess.check_output(['otool','-L',str(r/'build/gtk2_ardour/ardour-9.10.18')],text=True).splitlines()[1:]:
 p=line.strip().split(' (')[0]
 if p.startswith(('/opt/homebrew/',str(r))) and p.endswith('.dylib'):deps.append(p)
cmd=['/usr/bin/clang','-dynamiclib','-Wno-deprecated-declarations',*['-I'+str(p) for p in incs if p.is_dir()],str(r/'tools/dev/anticipation_ui_capture.c'),*deps,'-o',str(r/'artifacts/anticipation-gui/capture.dylib')]
sys.exit(subprocess.run(cmd).returncode)
