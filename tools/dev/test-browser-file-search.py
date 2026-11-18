#!/usr/bin/env python3
"""Compile separately, then run file-search behavior with a hard nine-second cap."""
import argparse,pathlib,subprocess,shlex,tempfile,signal,time,hashlib,json
root=pathlib.Path(__file__).resolve().parents[2];out=root/'artifacts/browser-file-search-test';out.mkdir(parents=True,exist_ok=True)
p=argparse.ArgumentParser();p.add_argument('--prepare',action='store_true');args=p.parse_args()
inputs=[root/'gtk2_ardour/browser_file_search.h',root/'gtk2_ardour/browser_file_filter.h',root/'tools/dev/browser_file_search_test.cc']
if args.prepare:
    flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','glib-2.0'],text=True))
    subprocess.run(['clang++','-std=c++17','-O2','-UNDEBUG',str(inputs[-1]),*flags,'-pthread','-o',str(out/'probe')],check=True)
    (out/'prepared.json').write_text(json.dumps({str(x):hashlib.sha256(x.read_bytes()).hexdigest() for x in inputs+[out/'probe']}));raise SystemExit

def timeout(*_):raise TimeoutError('Search test exceeded 9 seconds')
signal.signal(signal.SIGALRM,timeout);signal.alarm(9);start=time.monotonic()
for path,digest in json.loads((out/'prepared.json').read_text()).items():assert hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()==digest,'Reprepare stale search probe'
with tempfile.TemporaryDirectory(prefix='ardour-search-') as path:subprocess.run([str(out/'probe'),path],check=True,timeout=8)
signal.alarm(0);print(f'Elapsed: {time.monotonic()-start:.2f}s')
