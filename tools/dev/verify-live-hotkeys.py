#!/usr/bin/env python3
"""Exercise live config reload against a running test session via Ardour MCP.
Requires the branch's MCP UI tools. Restores the original config even on failure.
"""
import argparse, importlib.util, json, os, time
from pathlib import Path
spec=importlib.util.spec_from_file_location('iteration',Path(__file__).with_name('macos-fast-iterate.py'))
iteration=importlib.util.module_from_spec(spec);spec.loader.exec_module(iteration)
tool=iteration.tool
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--config',type=Path,default=Path.home()/'Library/Preferences/Ardour9/ardour.keys')
p.add_argument('--session-name',required=True)
a=p.parse_args()
assert tool('session_get_info')['sessionName']==a.session_name, 'Use a dedicated verification session'
original=a.config.read_text()
assert original.count('key="n" action="Main/AddTrackBus"')==1

def state():return tool('ui_get_state',action='Main/AddTrackBus')
def wait(predicate):
    start=time.perf_counter()
    while time.perf_counter()-start<5:
        if predicate():return time.perf_counter()-start
        time.sleep(.02)
    raise RuntimeError('State did not change within 5 seconds')
def dialog():return next((w['title'] for w in state()['windows'] if w['title']=='Add Track/Bus/VCA'),None)
def close():
    if dialog():
        tool('ui_close_dialog',window='Add Track/Bus/VCA')
        wait(lambda:not dialog())
def press(key):
    title=next(w['title'] for w in state()['windows'] if w['title'].endswith(' - Ardour'))
    tool('ui_press_key',key=key,window=title)
def write(s):
    tmp=a.config.with_name(a.config.name+'.test-tmp')
    tmp.write_text(s);os.replace(tmp,a.config)
results={}
try:
    close()
    wait(lambda:state()['key']=='n')
    press('n');wait(dialog);close()
    changed=original.replace('key="n" action="Main/AddTrackBus"','key="Primary-Level4-n" action="Main/AddTrackBus"')
    write(changed)
    results['atomic_edit_to_active_seconds']=wait(lambda:state()['key']=='Primary-Level4-n')
    press('Primary-Level4-n');wait(dialog);close()
    press('n');time.sleep(.1);assert not dialog(), 'Old key still opens dialog'
    write(changed+'<');time.sleep(1.2)
    assert state()['key']=='Primary-Level4-n'
    press('Primary-Level4-n');wait(dialog);close()
    results['invalid_xml_preserves_binding']=True
    write(original)
    results['restore_to_active_seconds']=wait(lambda:state()['key']=='n')
    press('n');wait(dialog);close()
    results['passed']=True
finally:
    if a.config.read_text()!=original:write(original)
print(json.dumps(results,indent=2))
