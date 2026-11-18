#!/usr/bin/env python3
"""Compile and run the independent browser filter contract, hard 9s total."""
import pathlib, signal, subprocess, tempfile, time
root = pathlib.Path(__file__).resolve().parents[2]
start = time.monotonic()
def timeout(*_):
    raise TimeoutError('Browser filter test exceeded 9 seconds')
signal.signal(signal.SIGALRM, timeout)
signal.alarm(9)
with tempfile.TemporaryDirectory(prefix='browser-filter-') as directory:
    binary = str(pathlib.Path(directory) / 'test')
    subprocess.run(['c++', '-std=c++11', str(root / 'tools/dev/browser_file_filter_test.cc'), '-o', binary], check=True, timeout=7)
    subprocess.run([binary], check=True, timeout=1)
signal.alarm(0)
print(f'Elapsed: {time.monotonic() - start:.2f}s')
