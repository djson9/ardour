#!/usr/bin/env python3
"""Compile the selected-device GUI probe against a configured native build."""
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
result = subprocess.run(['/usr/bin/clang++', *cache['CXXFLAGS'], '-dynamiclib', '-undefined', 'dynamic_lookup',
                         *['-I' + str(p) for p in includes if p.is_dir()], str(root / 'tools/dev/selected-device-ui-probe.cc'), '-o', str(output)])
if result.returncode:
    raise SystemExit(result.returncode)
print(output)
