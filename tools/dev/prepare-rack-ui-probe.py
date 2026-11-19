#!/usr/bin/env python3
"""Compile the C rack probe and C++ shared-row inspection bridge."""
import ast
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[2]
cache = {}
for line in (root / 'build/c4che/_cache.py').read_text().splitlines():
    if ' = ' in line:
        key, value = line.split(' = ', 1)
        try:
            cache[key] = ast.literal_eval(value)
        except (ValueError, SyntaxError):
            pass
includes = [root, root / 'build', root / 'gtk2_ardour', root / 'build/gtk2_ardour', root / 'libs/libltc/ltc']
includes += list((root / 'libs').iterdir()) + list((root / 'libs/tk').iterdir()) + list((root / 'build/libs').iterdir()) + list((root / 'build/libs/tk').iterdir())
for key, value in cache.items():
    if key.startswith('INCLUDES') and isinstance(value, list):
        includes.extend(map(Path, value))
output = Path(sys.argv[1]).resolve()
output.parent.mkdir(parents=True, exist_ok=True)
c_object = output.with_suffix('.c.o')
cpp_object = output.with_suffix('.cc.o')
include_flags = ['-I' + str(p) for p in includes if p.is_dir()]
subprocess.run(['/usr/bin/clang', '-Wno-deprecated-declarations', *include_flags, '-c', str(root / 'tools/dev/audio-rack-ui-probe.c'), '-o', str(c_object)], check=True)
native = ['-x', 'objective-c++'] if sys.platform == 'darwin' else []
frameworks = ['-framework', 'Cocoa'] if sys.platform == 'darwin' else []
subprocess.run(['/usr/bin/clang++', *cache['CXXFLAGS'], *native, *include_flags, '-c', str(root / 'tools/dev/audio-rack-row-probe.cc'), '-o', str(cpp_object)], check=True)
subprocess.run(['/usr/bin/clang++', '-dynamiclib', '-undefined', 'dynamic_lookup', *frameworks, str(c_object), str(cpp_object), '-o', str(output)], check=True)
print(output)
