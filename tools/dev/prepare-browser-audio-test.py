#!/usr/bin/env python3
"""Prepare the independent audio-analysis probe using a configured native build."""
import ast, hashlib, json, pathlib, subprocess
root=pathlib.Path(__file__).resolve().parents[2];out=root/'artifacts/browser-audio-analysis';out.mkdir(parents=True,exist_ok=True)
cache={}
for line in (root/'build/c4che/_cache.py').read_text().splitlines():
    if ' = ' in line:
        key,value=line.split(' = ',1)
        try:cache[key]=ast.literal_eval(value)
        except (ValueError,SyntaxError):pass
includes=[root,root/'build',root/'libs',root/'gtk2_ardour',root/'libs/appleutility/CoreAudio/PublicUtility']+list((root/'libs').iterdir())+list((root/'libs/tk').iterdir())+list((root/'build/libs').iterdir())
for key,value in cache.items():
    if key.startswith('INCLUDES') and isinstance(value,list):includes.extend(map(pathlib.Path,value))
exe=root/'build/gtk2_ardour'/('ardour-'+cache['VERSION'])
deps=[]
for line in subprocess.check_output(['otool','-L',str(exe)],text=True).splitlines()[1:]:
    path=line.strip().split(' (')[0]
    if path.startswith(('/opt/homebrew/',str(root))) and path.endswith('.dylib'):deps.append(path)
# libardour exports decoder types; the analysis uses AudioGrapher directly.
for path in [root/'build/libs/ardour/libardour.dylib',root/'build/libs/audiographer/libaudiographer.dylib']:
    if str(path) not in deps:deps.append(str(path))
sources=[root/'gtk2_ardour/browser_audio_analysis.cc',root/'tools/dev/browser_audio_analysis_test.cc']
command=['/usr/bin/clang++',*cache['CXXFLAGS'],'-std=c++17','-UNDEBUG','-Wno-deprecated-declarations','-Wno-inconsistent-missing-override',*['-I'+str(p) for p in dict.fromkeys(includes) if p.is_dir()],*map(str,sources),*deps,'-L/opt/homebrew/lib','-lsndfile','-framework','CoreFoundation','-framework','AppKit','-o',str(out/'probe')]
subprocess.run(command,check=True)
inputs=sources+[root/'gtk2_ardour/browser_audio_analysis.h',out/'probe']
(out/'prepared.json').write_text(json.dumps({str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}))
print(out/'probe')
