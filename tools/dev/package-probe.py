#!/usr/bin/env python3
"""Package only the isolated probe and its runtime for another arm64 Mac.

All install-name/signature changes apply to task-owned copies, never the build
or installed applications. Source and packaged hashes are retained separately.
"""
import hashlib, json, pathlib, shutil, subprocess, sys, tarfile

root = pathlib.Path(sys.argv[1]).resolve()
out = root / 'artifacts/portable-anticipation'
assert not out.exists(), 'Use a new destination rather than replacing a runtime'
runtime = out / 'runtime'
runtime.mkdir(parents=True)
def digest(path): return hashlib.sha256(path.read_bytes()).hexdigest()
def dependencies(path):
    return [line.strip().split(' (')[0] for line in subprocess.check_output(
        ['otool', '-L', str(path)], text=True).splitlines()[1:]]

probe_dir = root / 'artifacts/anticipation-callback-probe'
entries = [probe_dir / 'probe', probe_dir / 'private-home.dylib']
backends = list((root / 'build/libs/backends').glob('*/lib*backend.dylib'))
panners = list((root / 'build/libs/panners').glob('*/lib*.dylib'))
plugins = list((root / 'build/libs/LV2').glob('*/*.dylib'))
entries += backends + panners + plugins
pending = entries[:]
paths = {}
while pending:
    source = pending.pop().resolve()
    if source.name in paths:
        assert digest(source) == digest(paths[source.name]), 'basename collision'
        continue
    paths[source.name] = source
    for dep in dependencies(source):
        if dep.startswith(('/System/', '/usr/lib/')): continue
        if dep.startswith('@'): raise RuntimeError('Unresolved dependency: ' + dep)
        path = pathlib.Path(dep)
        assert path.exists(), dep
        pending.append(path)

manifest = {}
for name, source in paths.items():
    dest = runtime / name
    shutil.copy2(source, dest)
    dest.chmod(0o755)
    changes = []
    for dep in dependencies(source):
        if dep.startswith(('/System/', '/usr/lib/')): continue
        changes += ['-change', dep, '@executable_path/' + pathlib.Path(dep).resolve().name]
    if dest.suffix == '.dylib': changes += ['-id', '@executable_path/' + name]
    if changes: subprocess.run(['install_name_tool', *changes, str(dest)], check=True)
    subprocess.run(['codesign', '--force', '--sign', '-', str(dest)], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    manifest[name] = dict(source=str(source), source_sha256=digest(source), packaged_sha256=digest(dest))

for category, sources in [('backends', backends), ('panners', panners)]:
    directory = out / category
    directory.mkdir()
    for source in sources: (directory / source.name).symlink_to('../runtime/' + source.resolve().name)
shutil.copytree(root / 'share', out / 'share')
lv2 = out / 'LV2'
lv2.mkdir()
for source in plugins:
    directory = lv2 / source.parent.name
    directory.mkdir(exist_ok=True)
    for metadata in source.parent.glob('*.ttl'): shutil.copy2(metadata, directory / metadata.name)
    (directory / source.name).symlink_to('../../runtime/' + source.resolve().name)
manifest['_build'] = dict(commit=subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip(),
                         source_sha256={p.name: digest(p) for p in (root / 'tools/dev').glob('anticipation*probe.cc')})
(out / 'manifest.json').write_text(json.dumps(manifest, indent=2))
with tarfile.open(str(out) + '.tar.gz', 'w:gz') as archive: archive.add(out, arcname=out.name)
print(str(out) + '.tar.gz')
