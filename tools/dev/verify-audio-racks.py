#!/usr/bin/env python3
"""Run the focused native tests with isolated macOS preferences.
Requires a configured --test build and the dummy backend.
Use --prepare-gui after passing tests to create a disposable GUI fixture/job.
The script prepares the job; launch it explicitly with launchctl bootstrap.
"""
import argparse, ast, os, pathlib, plistlib, shlex, subprocess, xml.etree.ElementTree as ET
root=pathlib.Path(__file__).resolve().parents[2]
out=root/'artifacts/audio-racks'
p=argparse.ArgumentParser();p.add_argument('--prepare-gui',action='store_true');p.add_argument('--embedded',action='store_true');args=p.parse_args()
out.mkdir(parents=True,exist_ok=True)
home=out/('private-home' if args.prepare_gui else 'engine-home');prefs=home/'Library/Preferences/Ardour9';prefs.mkdir(parents=True,exist_ok=True)
interpose=out/'private-home.dylib'
subprocess.run(['/usr/bin/clang','-dynamiclib','-undefined','dynamic_lookup',str(root/'tools/dev/test-private-home.c'),'-o',str(interpose)],check=True)
common='TOP='+shlex.quote(str(root))+'\n. "$TOP/build/gtk2_ardour/ardev_common_waf.sh"\n'
env=os.environ.copy();env['ARDOUR_PROBE_HOME']=str(home)
if not args.prepare_gui:
 command=common+'export DYLD_INSERT_LIBRARIES='+shlex.quote(str(interpose))+'\nexec "$TOP/build/libs/ardour/test_audio_racks"'
 with (out/'engine-tests.log').open('w') as log:
  result=subprocess.run(['/bin/bash','-c',command],env=env,stdout=log,stderr=subprocess.STDOUT,timeout=90)
 print((out/'engine-tests.log').read_text());raise SystemExit(result.returncode)
log=(out/'engine-tests.log').read_text()
assert 'OK (5)' in log, 'Run and pass the engine tests first'
source=pathlib.Path(next(line.split('Rack fixture: ',1)[1] for line in log.splitlines() if 'Rack fixture: ' in line))/'test_session.ardour'
fixture=out/'Rack Verification';fixture.mkdir(exist_ok=True)
tree=ET.parse(source);session=tree.getroot();session.set('name','Rack Verification')
protocols=session.find('ControlProtocols')
if protocols is None:protocols=ET.SubElement(session,'ControlProtocols')
protocols.clear();ET.SubElement(protocols,'Protocol',name='MCP HTTP Server (Experimental)',active='1',port='4838',config='')
route=next(r for r in session.findall('./Routes/Route') if r.find("Processor[@type='audio-rack']") is not None)
rack=route.find("Processor[@type='audio-rack']")
extra=session.find('Extra')
if extra is None:extra=ET.SubElement(session,'Extra')
ui=extra.find('UI')
if ui is None:ui=ET.SubElement(extra,'UI')
ET.SubElement(ui,'Window',{'name':f"P-{route.get('id')}-{rack.get('id')}",'visible':'0' if args.embedded else '1','custom-ui':'1','x-size':'1080','y-size':'280'})
editor=ET.SubElement(ui,'Editor');editor.set('show-editor-props','1')
tree.write(fixture/'Rack Verification.ardour',encoding='UTF-8',xml_declaration=True)
config=ET.Element('Ardour');options=ET.SubElement(config,'Config')
for name,value in {'check-announcements':'0','show-splash':'0','discover-plugins-on-start':'0','default-session-parent-dir':str(out),'anticipative-processing':'0'}.items(): ET.SubElement(options,'Option',name=name,value=value)
protocols=ET.SubElement(config,'ControlProtocols');ET.SubElement(protocols,'Protocol',name='MCP HTTP Server (Experimental)',active='1',port='4838',config='')
states=ET.SubElement(ET.SubElement(ET.SubElement(config,'Extra'),'AudioMIDISetup'),'EngineStates')
ET.SubElement(states,'State',{'backend':'None (Dummy)','driver':'Realtime','device':'Silence','input-device':'Silence','output-device':'Silence','sample-rate':session.get('sample-rate','48000'),'buffer-size':'256','n-periods':'2','input-latency':'0','output-latency':'0','active':'1','use-buffered-io':'0','midi-option':'None','lru':'1'})
ET.ElementTree(config).write(prefs/'config',encoding='UTF-8',xml_declaration=True);(prefs/'.a9').touch()
cache={}
for line in (root/'build/c4che/_cache.py').read_text().splitlines():
 if ' = ' in line:
  key,value=line.split(' = ',1)
  try:cache[key]=ast.literal_eval(value)
  except (ValueError,SyntaxError):pass
incs=list((root/'libs/tk').iterdir())+list((root/'build/libs/tk').iterdir())
for key,value in cache.items():
 if key.startswith('INCLUDES') and isinstance(value,list):incs.extend(map(pathlib.Path,value))
probe=out/'ui-probe.dylib'
subprocess.run(['/usr/bin/clang','-dynamiclib','-undefined','dynamic_lookup','-Wno-deprecated-declarations',*['-I'+str(d) for d in incs if d.is_dir()],str(root/'tools/dev/audio-rack-ui-probe.c'),'-o',str(probe)],check=True)
exe=root/'build/gtk2_ardour'/('ardour-'+cache['VERSION'])
command=common+'export DYLD_INSERT_LIBRARIES='+shlex.quote(str(interpose)+':'+str(probe))+'\nexec '+shlex.join([str(exe),'-a','-n',str(fixture)])
job=dict(Label='dev.ardour.audio-racks-verification',ProgramArguments=['/bin/bash','-c',command],RunAtLoad=True,EnvironmentVariables={'ARDOUR_PROBE_HOME':str(home),'ARDOUR_RACK_UI_PROBE':str(out),'ARDOUR_LIVE_LAYOUT':'1'},StandardOutPath=str(out/'gui.log'),StandardErrorPath=str(out/'gui.log'))
if args.embedded:job['EnvironmentVariables']['ARDOUR_RACK_UI_EMBEDDED']='1'
(out/'gui.plist').write_bytes(plistlib.dumps(job));print(out/'gui.plist')
