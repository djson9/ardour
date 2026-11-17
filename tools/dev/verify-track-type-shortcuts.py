#!/usr/bin/env python3
"""Check numbered Add Track choices through MCP in an English verification session."""
import argparse, importlib.util, json, subprocess, sys, time
from pathlib import Path
spec=importlib.util.spec_from_file_location('iteration',Path(__file__).with_name('macos-fast-iterate.py'))
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--session-name',required=True)
a=p.parse_args()
assert m.tool('session_get_info')['sessionName']==a.session_name
if sys.platform=='darwin':
    subprocess.run(['osascript','-e','tell application "Ardour9" to activate'],check=True)
    time.sleep(.2)
DIALOG='Add Track/Bus/VCA'
def windows():return m.tool('ui_get_state',action='Main/AddTrackBus')['windows']
def dialog():return next((w for w in windows() if w['title']==DIALOG),None)
def wait(predicate):
    deadline=time.monotonic()+3
    while time.monotonic()<deadline:
        if predicate():return
        time.sleep(.02)
    raise RuntimeError('UI state did not match: '+str(windows()))
def key(value,window=DIALOG):m.tool('ui_press_key',key=value,window=window)
def close():
    if dialog():m.tool('ui_close_dialog',window=DIALOG);wait(lambda:not dialog())
original_tracks=m.tool('tracks_list')['tracks']
close()
main=next(w['title'] for w in windows() if w['title'].endswith(' - Ardour'))
key('n',main);wait(dialog)
assert dialog()['focusName']=='AddRouteDialogTypeChooser'
names=['Audio Tracks','MIDI Tracks','Audio Busses','MIDI Busses','VCA Masters','Foldback Busses']
for i,name in enumerate(names,1):
    key(str(i));wait(lambda:dialog().get('selectedChoice')==name)
    assert dialog()['focusName']=='AddRouteDialogTypeChooser'
key('KP_1');wait(lambda:dialog().get('selectedChoice')==names[0])
# Tab reaches the quantity input. Numeric input must not change track type.
for _ in range(12):
    key('Tab');time.sleep(.02)
    if dialog()['textEntry']:break
assert dialog()['textEntry'], 'Quantity input did not receive focus'
key('Primary-a');key('2');wait(lambda:dialog().get('focusText')=='2')
key('Primary-a');key('1');wait(lambda:dialog().get('focusText')=='1')
# The next Tab reaches the name. Digits remain normal text there as well.
key('Tab');wait(lambda:dialog()['focusName']=='AddRouteDialogNameTemplateEntry')
original_name=dialog()['focusText']
key('Primary-a');key('3');wait(lambda:dialog().get('focusText')=='3')
key('Primary-a')
for char in original_name:key(char.lower())
for _ in range(12):
    key('Tertiary-Tab');time.sleep(.02)
    if dialog()['focusName']=='AddRouteDialogTypeChooser':break
assert dialog()['focusName']=='AddRouteDialogTypeChooser'
assert dialog()['selectedChoice']==names[0]
assert m.tool('tracks_list')['tracks']==original_tracks, 'Selecting a type created a track'
close();key('n',main);wait(dialog)
assert dialog()['focusName']=='AddRouteDialogTypeChooser'
key('2');wait(lambda:dialog().get('selectedChoice')==names[1])
close()
print(json.dumps(dict(passed=True,choices=names,keypad=True,text_input_preserved=True,no_tracks_created=True,reopen_focus=True),indent=2))
