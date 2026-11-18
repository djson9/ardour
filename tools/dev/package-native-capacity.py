"""Relocate the isolated native probe and its dylibs for another arm64 test Mac."""
import pathlib,subprocess,shutil,json,hashlib,os
root=pathlib.Path(__file__).resolve().parents[2];out=root/'artifacts/native-capacity-portable';out.mkdir(exist_ok=True)
probe=root/'artifacts/anticipation-native-capacity-probe/probe'
files={};pending=[]
def add(src,dst=None):
 src=pathlib.Path(src).resolve()
 if src in files:return files[src]
 dst=dst or pathlib.Path('lib')/src.name;files[src]=dst;pending.append(src);target=out/dst;target.parent.mkdir(parents=True,exist_ok=True);target.unlink(missing_ok=True);shutil.copy2(src,target);target.chmod(target.stat().st_mode|0o200);return dst
add(probe,pathlib.Path('probe'))
add(root/'artifacts/anticipation-native-capacity-probe/private-home.dylib',pathlib.Path('private-home.dylib'))
for backend in ('coreaudio','dummy'):
 for src in (root/'build/libs/backends'/backend).glob('*.dylib'):add(src,pathlib.Path('backends')/src.name)
for src in (root/'build/libs/panners').rglob('*.dylib'):add(src,pathlib.Path('panners')/src.name)
shutil.copytree(root/'build/libs/LV2',out/'LV2',dirs_exist_ok=True)
for src in (root/'build/libs/LV2').rglob('*.dylib'):add(src,pathlib.Path('LV2')/src.relative_to(root/'build/libs/LV2'))
shutil.copytree(root/'share/plugin_metadata',out/'share/plugin_metadata',dirs_exist_ok=True)
manifest=[]
while pending:
 src=pending.pop();dst=files[src];target=out/dst
 deps=subprocess.check_output(['otool','-L',str(src)],text=True).splitlines()[1:];changes=[]
 for row in deps:
  dep=row.strip().split(' (',1)[0]
  if dep.startswith(('/System/','/usr/lib/')):continue
  p=pathlib.Path(dep)
  if dep.startswith('@loader_path/'):p=src.parent/dep[len('@loader_path/'):]
  if dep.startswith('@rpath/'):
   candidates=list((root/'build').rglob(pathlib.Path(dep).name))+list(pathlib.Path('/opt/homebrew/lib').glob(pathlib.Path(dep).name));assert candidates,dep;p=candidates[0]
  assert p.exists(),dep
  to=add(p);relative='@loader_path/'+os.path.relpath(to,dst.parent);changes+=['-change',dep,relative]
 if changes:subprocess.run(['install_name_tool',*changes,str(target)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
 subprocess.run(['codesign','--force','--sign','-',str(target)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
 manifest.append(dict(source=str(src),path=str(dst),original_sha256=hashlib.sha256(src.read_bytes()).hexdigest(),relocated_sha256=hashlib.sha256(target.read_bytes()).hexdigest()))
(out/'manifest.json').write_text(json.dumps(dict(source_commit=subprocess.check_output(['git','rev-parse','HEAD'],text=True,cwd=root).strip(),files=manifest,probe_sources={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in (root/"tools/dev").glob("anticipation*probe.cc")}),indent=2))
subprocess.run(['tar','-czf',str(root/'artifacts/native-capacity-portable.tar.gz'),'-C',str(out),'.'],check=True)
print(len(files),'binaries packaged')
