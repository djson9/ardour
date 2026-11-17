#!/usr/bin/env python3
"""Incremental macOS build/deploy. Run prepare on the build host, iterate on the desktop.
Requires an already packaged app; dependencies added to changed binaries are bundled.
Only Mach-O files are updated: use normal packaging after resource/layout changes.
"""
import argparse, concurrent.futures, json, os, plistlib, re, shlex, shutil, signal, subprocess, sys, tarfile, time, urllib.request
from pathlib import Path


def run(args, **kw):
    return subprocess.run([str(a) for a in args], check=True, **kw)


def rpc(method, params=None):
    request = urllib.request.Request('http://127.0.0.1:4820/mcp', json.dumps(dict(jsonrpc='2.0',id=1,method=method,params=params or {})).encode(), {'Content-Type':'application/json','Accept':'application/json'})
    with urllib.request.urlopen(request, timeout=3) as response:
        result = json.load(response)
    if 'error' in result: raise RuntimeError(result['error'])
    return result['result']


def tool(tool_name, **args):
    result = rpc('tools/call',dict(name=tool_name,arguments=args))
    if result.get('isError'): raise RuntimeError(result)
    data=result.get('structuredContent',result)
    if 'error' in data: raise RuntimeError(data['error'])
    return data


def normalized(name):
    return re.sub(r'(?:\.\d+)+\.dylib$', '.dylib', name)


def prepare(args):
    root = Path(args.source).resolve()
    app = Path(args.app).resolve()
    state_dir = root/'build/fast-iterate'
    state_dir.mkdir(exist_ok=True)
    manifest_path = state_dir/'manifest.json'
    old = json.loads(manifest_path.read_text()) if manifest_path.exists() else {}
    mapping = {}
    build_files=list((root/'build').rglob('*')) if args.all or not old else [Path(p) for p in old if Path(p).is_relative_to(root/'build') and Path(p).is_file()]
    app_files={}
    for entry in app.rglob('*'):
        if entry.is_file(): app_files.setdefault(entry.name,entry)
    for source in build_files:
        if source.suffix != '.dylib': continue
        if source.is_symlink(): continue
        if 'fast-iterate' in source.parts: continue
        name = normalized(source.name)
        parent = 'Contents/lib'
        if '/surfaces/' in str(source): parent += '/surfaces'
        elif '/backends/' in str(source): parent += '/backends'
        elif '/panners/' in str(source): parent += '/panners'
        elif name == 'libclearlooks.dylib': parent += '/gtkengines'
        dest = parent+'/'+name
        # Some upstream libraries intentionally retain ABI versions in bundles.
        candidate=app_files.get(source.name)
        if candidate: dest = str(candidate.relative_to(app))
        mapping[source.resolve()] = dest
    main = [p for p in (root/'build/gtk2_ardour').glob('ardour-[0-9]*') if p.is_file() and not p.is_symlink()]
    if not main: raise RuntimeError('No compiled Ardour executable')
    mapping[max(main,key=lambda p:p.stat().st_mtime)] = 'Contents/MacOS/Ardour9'
    for name in ['ardour-au-scanner','ardour-vst-scanner','ardour-vst3-scanner','ardour-exec-wrapper','ardour9-export','ardour9-lua','ardour9-new_session','ardour9-new_empty_session']:
        for p in build_files:
            if p.name == name and p.is_file() and 'fast-iterate' not in p.parts:
                if p.read_bytes()[:4] == b'\xcf\xfa\xed\xfe': mapping[p.resolve()] = 'Contents/lib/'+name
    for source, entry in old.items():
        p=Path(source)
        if p.is_file() and not p.is_relative_to(root/'build'):
            mapping[p]=entry[2]
    new = {str(p):[p.stat().st_mtime_ns,p.stat().st_size,dest] for p,dest in mapping.items()}
    changed = {p:dest for p,dest in mapping.items() if args.all or old.get(str(p)) != new[str(p)]}
    payload = state_dir/'payload'
    if payload.exists(): shutil.rmtree(payload)
    payload.mkdir()
    aliases = {p.name:dest for p,dest in mapping.items()}
    aliases.update({normalized(p.name):dest for p,dest in mapping.items()})
    bundled = {name:str(p.relative_to(app)) for name,p in app_files.items() if p.is_relative_to(app/'Contents/lib')}
    pending = dict(changed)
    done = set()
    while pending:
        items = list(pending.items()); pending = {}
        for source,dest in items:
            if dest in done: continue
            target = payload/dest
            target.parent.mkdir(parents=True,exist_ok=True)
            shutil.copy2(source,target); target.chmod(0o755)
            done.add(dest)
        def fix(item):
            source,dest = item
            target = payload/dest
            deps = subprocess.check_output(['/usr/bin/otool','-L',str(target)],text=True).splitlines()[1:]
            changes=[]; additions={}
            for line in deps:
                dep = line.strip().split(' (',1)[0]
                if dep.startswith(('/usr/lib/','/System/Library/')): continue
                name = Path(dep).name
                mapped = aliases.get(name) or bundled.get(name)
                if not mapped:
                    found = Path(dep) if dep.startswith('/') else Path('/opt/homebrew/lib')/name
                    if not found.exists(): raise RuntimeError('Unresolved dependency '+dep)
                    mapped='Contents/lib/'+name
                    additions[found.resolve()]=mapped
                relative = '@loader_path/'+os.path.relpath(mapped,Path(dest).parent)
                if dep != relative: changes += ['-change',dep,relative]
            if changes: run(['/usr/bin/install_name_tool',*changes,target],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
            run(['/usr/bin/codesign','--force','--sign','-',target],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
            return additions
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            for additions in pool.map(fix,items):
                for source,dest in additions.items():
                    if dest not in done: pending[source]=dest
                    bundled[source.name]=dest
                    new[str(source)]=[source.stat().st_mtime_ns,source.stat().st_size,dest]
    archive = state_dir/'delta.tar'
    with tarfile.open(archive,'w') as out:
        for name in sorted(done): out.add(payload/name,arcname=name)
    # Only commit the manifest after the desktop confirms successful deployment.
    (state_dir/'manifest.pending.json').write_text(json.dumps(new))
    print(json.dumps(dict(archive=str(archive),files=sorted(done))))


def iterate(args):
    start = time.perf_counter()
    timings = {}
    remote_source = args.source
    ssh_options = ['-o','BatchMode=yes','-o','ControlMaster=auto','-o','ControlPersist=60','-o','ControlPath=/tmp/ardour-dev-%C']
    remote_script = remote_source+'/tools/dev/macos-fast-iterate.py'
    command = 'cd '+shlex.quote(remote_source)+' && '+('ARDOUR_DEV_FAST=1 ' if args.fast_compile else '')+'PATH=/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin python3 waf build -j'+str(args.jobs)
    if args.target: command += ' --targets='+shlex.quote(args.target)
    if args.restart:
        info=tool('session_get_info')
        if Path(info['sessionPath']).resolve() != Path(args.session).resolve().parent:
            raise RuntimeError('Active session differs from --session; refusing to switch projects')
        transport=tool('transport_get_state')
        if transport.get('rolling') or transport.get('recording') or float(transport.get('speed',0)) != 0:
            raise RuntimeError('Stop playback/recording before iterating')
        tool('session_save')
        if args.terminate_saved:
            pattern='^'+re.escape(str(Path(args.app).resolve()/'Contents/MacOS/Ardour9'))
            processes=subprocess.run(['pgrep','-f',pattern],capture_output=True,text=True)
            pids=processes.stdout.split()
            if len(pids)!=1:raise RuntimeError('Expected exactly one process for the specified app')
            os.kill(int(pids[0]),signal.SIGTERM)
        else:
            # Normal application quit may present a dialog; never force it.
            subprocess.Popen(['osascript','-e','tell application "Ardour9" to quit'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    run(['ssh',*ssh_options,args.host,command],stdout=sys.stderr)
    timings['build']=time.perf_counter()-start
    prep = ['python3',remote_script,'prepare','--source',remote_source,'--app',args.remote_app]
    if args.all: prep += ['--all']
    prepare_start=time.perf_counter()
    result = json.loads(subprocess.check_output(['ssh',*ssh_options,args.host,shlex.join(prep)],text=True).splitlines()[-1])
    prepare_seconds=time.perf_counter()-prepare_start
    transfer_start=time.perf_counter()
    local = Path(args.app).resolve()
    stage = local.parent/'.ardour-fast-iterate'
    stage.mkdir(exist_ok=True)
    archive = stage/'delta.tar'
    run(['rsync','-az','--no-whole-file','-e',shlex.join(['ssh',*ssh_options]),args.host+':'+result['archive'],archive])
    timings['prepare']=prepare_seconds
    timings['transfer']=time.perf_counter()-transfer_start
    if not result['files'] and not args.restart:
        print(json.dumps(dict(changed=False,seconds=time.perf_counter()-start))); return
    if args.restart:
        deadline=time.monotonic()+5
        while subprocess.run(['pgrep','-f','^'+str(local/'Contents/MacOS/Ardour9')],stdout=subprocess.DEVNULL).returncode == 0:
            if time.monotonic()>deadline: raise RuntimeError('Ardour did not quit; resolve its dialog and retry')
            time.sleep(.025)
    elif subprocess.run(['pgrep','-f','^'+str(local/'Contents/MacOS/Ardour9')],stdout=subprocess.DEVNULL).returncode == 0:
        raise RuntimeError('Quit Ardour or use --restart before replacing binaries')
    with tarfile.open(archive) as payload:
        for item in payload.getmembers():
            if not item.isfile() or not item.name.startswith('Contents/') or '..' in Path(item.name).parts: raise RuntimeError('Invalid payload member')
            dest=local/item.name; dest.parent.mkdir(parents=True,exist_ok=True)
            temp=dest.with_name(dest.name+'.new')
            with payload.extractfile(item) as src, temp.open('wb') as dst: shutil.copyfileobj(src,dst)
            temp.chmod(0o755); os.replace(temp,dest)
    run(['/usr/bin/codesign','--force','--sign','-',local],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    def finish_install():
        run(['/usr/bin/codesign','--verify','--deep','--strict',local],stdout=subprocess.DEVNULL)
        run(['ssh',*ssh_options,args.host,'mv '+shlex.quote(remote_source+'/build/fast-iterate/manifest.pending.json')+' '+shlex.quote(remote_source+'/build/fast-iterate/manifest.json')])
    validation_pool=concurrent.futures.ThreadPoolExecutor(max_workers=1)
    validation=validation_pool.submit(finish_install)
    timings['install']=time.perf_counter()-start-sum(timings.values())
    if args.restart:
        if args.direct_launch:
            env=os.environ.copy()
            with (local/'Contents/Info.plist').open('rb') as f: env.update(plistlib.load(f).get('LSEnvironment',{}))
            subprocess.Popen([str(local/'Contents/MacOS/Ardour9'),args.session],env=env,stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,start_new_session=True)
        else:
            run(['open','-a',local,'--args',args.session])
        deadline=time.monotonic()+12
        while True:
            try:
                ready=tool('ui_get_state',action='Main/AddTrackBus')
                if ready.get('sensitive') and any(w['title'].endswith(' - Ardour') for w in ready.get('windows',[])): break
            except (OSError,ValueError): pass
            if time.monotonic()>deadline: raise RuntimeError('Ardour did not become ready')
            time.sleep(.025)
        timings['launch_ready']=time.perf_counter()-start-sum(timings.values())
    validation.result()
    validation_pool.shutdown()
    timings['total']=time.perf_counter()-start
    print(json.dumps(dict(files=result['files'],seconds=timings),indent=2))


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('mode',choices=['prepare','iterate'])
    p.add_argument('--source',required=True)
    p.add_argument('--app',required=True)
    p.add_argument('--all',action='store_true')
    p.add_argument('--host')
    p.add_argument('--remote-app')
    p.add_argument('--target')
    p.add_argument('--fast-compile',action='store_true',help='Use development optimization for MCP, playlist transactions and warp adapter (not DSP)')
    p.add_argument('--jobs',type=int,default=8)
    p.add_argument('--restart',action='store_true')
    p.add_argument('--terminate-saved',action='store_true',help='Terminate the exact app process after verifying and saving its session; for authorized development restarts')
    p.add_argument('--direct-launch',action='store_true',help='Launch the development app directly with its bundle environment')
    p.add_argument('--session')
    args=p.parse_args()
    if args.mode=='iterate' and (not args.host or not args.remote_app or (args.restart and not args.session)): p.error('iterate needs --host, --remote-app; --restart also needs --session')
    (prepare if args.mode=='prepare' else iterate)(args)
if __name__=='__main__': main()
