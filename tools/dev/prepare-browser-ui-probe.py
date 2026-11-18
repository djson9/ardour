#!/usr/bin/env python3
"""Compile the disposable GTK browser probe using an existing native build.
Run on the build host, then inject the emitted dylib only into a Verification
app. No GUI is launched and no user configuration is changed by this command.
"""
import ast, pathlib, subprocess, sys
root=pathlib.Path(__file__).resolve().parents[2]
output=pathlib.Path(sys.argv[1]).resolve();output.parent.mkdir(parents=True,exist_ok=True)
cache={}
for line in (root/'build/c4che/_cache.py').read_text().splitlines():
    if ' = ' in line:
        key,value=line.split(' = ',1)
        try:cache[key]=ast.literal_eval(value)
        except (ValueError,SyntaxError):pass
includes=list((root/'libs/tk').iterdir())+list((root/'build/libs/tk').iterdir())
for key,value in cache.items():
    if key.startswith('INCLUDES') and isinstance(value,list):includes.extend(map(pathlib.Path,value))
subprocess.run(['/usr/bin/clang','-dynamiclib','-undefined','dynamic_lookup','-Wno-deprecated-declarations',*['-I'+str(d) for d in includes if d.is_dir()],str(root/'tools/dev/browser_ui_probe.c'),'-o',str(output)],check=True)
print(output)
