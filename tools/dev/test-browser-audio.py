#!/usr/bin/env python3
"""Run the prepared audio-analysis probe with a hard 9-second total deadline."""
import hashlib,json,os,pathlib,shlex,signal,subprocess,tempfile,time
root=pathlib.Path(__file__).resolve().parents[2];out=root/'artifacts/browser-audio-analysis'
def timeout(*_):raise TimeoutError('Browser audio test exceeded 9 seconds')
signal.signal(signal.SIGALRM,timeout);signal.alarm(9);start=time.monotonic()
for path,expected in json.loads((out/'prepared.json').read_text()).items():assert hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()==expected,'Reprepare stale analysis probe'
with tempfile.TemporaryDirectory(prefix='browser-audio-') as directory:
    command='TOP='+shlex.quote(str(root))+'\n. "$TOP/build/gtk2_ardour/ardev_common_waf.sh"\nexec '+shlex.join([str(out/'probe'),directory])
    subprocess.run(['/bin/bash','-c',command],check=True,timeout=8)
signal.alarm(0);print(f'Elapsed: {time.monotonic()-start:.2f}s')
