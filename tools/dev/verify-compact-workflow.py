#!/usr/bin/env python3
"""Nine-second native GTK checks. Use a disposable session named *Verification*."""
import argparse, json, signal, subprocess, time, urllib.request
import xml.etree.ElementTree as ET
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--url', default='http://127.0.0.1:4838/mcp')
p.add_argument('--session-path', type=Path, required=True)
p.add_argument('--case', choices=['headers', 'search', 'scroll', 'curves'], required=True)
a = p.parse_args()

def call(name, **args):
    body = dict(jsonrpc='2.0', id=1, method='tools/call', params=dict(name=name, arguments=args))
    request = urllib.request.Request(a.url, json.dumps(body).encode(), {'Content-Type': 'application/json'})
    reply = json.load(urllib.request.urlopen(request, timeout=2))
    assert 'error' not in reply, reply
    result = reply['result']
    assert not result.get('isError'), result
    result = result.get('structuredContent', result)
    assert 'error' not in result, result
    return result

def windows(): return call('ui_get_state')['windows']
def main(): return next(w for w in windows() if w['title'].endswith(' - Ardour'))
def key(k, window=None): return call('ui_press_key', key=k, window=window or main()['title'])
def pointer(canvas, event, x, y, **kw):
    result = call('ui_canvas_pointer', window=main()['title'], canvas=canvas, event=event, x=x, y=y, **kw)
    assert result.get('delivered'), result

def header(name, **kw):
    for e in ['press', 'release']: pointer('TrackHeader:' + name, e, 30, 14, **kw)

def wait(predicate):
    end = time.monotonic() + 1.5
    while time.monotonic() < end:
        result = predicate()
        if result: return result
        time.sleep(.015)
    raise AssertionError('UI did not reach the expected state')

def routes(): return call('tracks_list', includeHidden=True)['tracks']
def instant():
    call('ui_activate_action', action='Common/Save')
    time.sleep(.025)
    return ET.parse(a.session_path / 'instant.xml').find('Editor')
def selected(): return {e.get('id') for e in instant().findall('Selection/StripableView')}
def expect_selection(ids): wait(lambda: selected() == set(ids))
def create(shortcut, name, kind):
    old = {r['id'] for r in routes()}
    key(shortcut)
    new = wait(lambda: [r for r in routes() if r['id'] not in old])
    assert len(new) == 1 and new[0]['type'] == kind, new
    assert not any(w['title'] == 'Add Track/Bus/VCA' for w in windows())
    call('track_rename', id=new[0]['id'], newName=name)
    return new[0]['id']

def remove_fixture(names, ids):
    header(names[0])
    for name in names[1:]: header(name, primary=True)
    expect_selection(ids)
    key('BackSpace')
    dialog = wait(lambda: next((w for w in windows() if w['modal']), None))
    assert {r['id'] for r in routes()}.issuperset(ids)
    key('Tab', dialog['title']); key('Return', dialog['title'])
    wait(lambda: not ({r['id'] for r in routes()} & set(ids)))

assert 'verification' in a.session_path.name.lower(), 'Use a disposable verification session'
assert Path(call('session_get_info')['sessionPath']).resolve() == a.session_path.resolve()
signal.signal(signal.SIGALRM, lambda *_: (_ for _ in ()).throw(TimeoutError('9-second test deadline')))
signal.alarm(9)
start = time.monotonic()
# Tests use GTK events; activation allows native dialogs/completion to own focus.
port = a.url.split(':')[-1].split('/')[0]
pid = subprocess.check_output(['lsof', '-t', '-iTCP:' + port, '-sTCP:LISTEN'], text=True).strip()
subprocess.run(['osascript', '-e', 'tell application "System Events" to tell (first application process whose unix id is ' + pid + ') to set frontmost to true'], check=True)
header(next(r['name'] for r in routes() if not r['hidden'] and r['type'].endswith('_track')))
checks = []

if a.case == 'headers':
    names = ['UI Test Selection ' + str(i) for i in range(1, 4)]
    assert not any(r['name'] in names for r in routes()), 'Remove prior test fixtures first'
    ids = [create('Primary-Tertiary-t', n, 'midi_track') for n in names]
    header(names[0]); expect_selection(ids[:1])
    header(names[2], shift=True); expect_selection(ids)
    header(names[1], primary=True); expect_selection([ids[0], ids[2]])
    checks += ['single', 'shift range', 'command toggle']
    key('Primary-a')
    expect_selection([r['id'] for r in routes() if not r['hidden'] and r['type'].endswith('_track')])
    before = {r['id'] for r in routes()}
    key('BackSpace')
    dialog = wait(lambda: next((w for w in windows() if w['modal']), None))
    assert {r['id'] for r in routes()} == before
    call('ui_close_dialog', window=dialog['title'])
    wait(lambda: not any(w['modal'] for w in windows()))
    assert {r['id'] for r in routes()} == before
    checks += ['command A', 'delete asks first', 'cancel preserves tracks']
    remove_fixture(names, ids)
    checks += ['confirm removes only selected test tracks']

elif a.case == 'search':
    names = ['UI Search Alpha', 'UI Search Bravo Long']
    assert not any(r['name'] in names for r in routes()), 'Remove prior test fixtures first'
    ids = [create('Primary-t', names[0], 'audio_track'), create('Primary-Tertiary-t', names[1], 'midi_track')]
    key('slash')
    title = wait(lambda: next((w['title'] for w in windows() if w['title'] == 'Find Track/Bus'), None))
    for ch in 'ui search': key('space' if ch == ' ' else ch, title)
    key('Primary-a', title)
    entry = wait(lambda: next((w for w in windows() if w['title'] == title and w.get('selectionEnd') == len('ui search')), None))
    assert entry['selectionStart'] == 0
    time.sleep(.35)  # GTK's completion popup delay, within the overall deadline.
    key('Down', title); key('Return', title)
    wait(lambda: not any(w['title'] == title for w in windows()))
    expect_selection([ids[1]])
    key('slash'); wait(lambda: any(w['title'] == title for w in windows()))
    for ch in 'ui search': key('space' if ch == ' ' else ch, title)
    time.sleep(.35)
    key('Down', title)  # Move off the first result, then refine the query.
    key('space', title)
    time.sleep(.35)
    key('Return', title)
    wait(lambda: not any(w['title'] == title for w in windows()))
    expect_selection([ids[0]])
    header(names[0]); expect_selection([ids[0]])
    key('slash'); wait(lambda: any(w['title'] == title for w in windows()))
    for ch in 'brvo': key(ch, title)
    key('Return', title)
    wait(lambda: not any(w['title'] == title for w in windows()))
    expect_selection([ids[1]])
    checks += ['command T audio', 'command shift T MIDI', 'slash opens search', 'first result selected; Down chooses second', 'typing resets selection to first', 'typo match', 'command A edits text in search']
    remove_fixture(names, ids)

elif a.case == 'curves':
    name = 'UI Curve Fixture'
    assert not any(r['name'] == name for r in routes()), 'Remove prior test fixtures first'
    call('transport_stop')
    call('transport_locate_sample', sample=0)
    old = {r['id'] for r in routes()}
    call('ui_activate_action', action='Editor/add-automation-track')
    route = wait(lambda: next((r for r in routes() if r['id'] not in old), None))
    call('track_rename', id=route['id'], newName=name)
    call('ui_activate_action', action='EditorEditing/set-mouse-mode-object')
    region = wait(lambda: call('track_get_regions', id=route['id'])['regions'])[0]
    time.sleep(.1)
    header(name)
    call('ui_activate_action', action='Editor/zoom-to-session')
    pointer('EditorMainCanvas', 'scroll', 100, 100, deltaY=1000)
    time.sleep(.1)
    def clip_box():
        canvas = next(c for c in main()['canvases'] if c['name'] == 'EditorMainCanvas')
        labels = [l for l in canvas['labels'] if l['text'] == 'Automation']
        return max(labels, key=lambda l: l['y0'])['ancestors'][0]
    box = clip_box()
    pointer('EditorMainCanvas', 'scroll', 100, 100, deltaY=box['y0'] - 100)
    time.sleep(.03)
    box = clip_box()
    x, y = (box['x0'] + box['x1']) / 2, (box['y0'] + box['y1']) / 2
    for event in ['move', 'press', 'doublePress', 'release']:
        pointer('EditorMainCanvas', event, x, y)
    c = wait(lambda: next((c for c in main()['canvases'] if c['name'] == 'AutomationClipCanvas'), None))
    def point(t, v): return 36 + t * (c['width'] - 52), 30 + (1-v) * (c['height'] - 46)
    def cp(event, t, v, **kw): pointer('AutomationClipCanvas', event, *point(t, v), **kw)
    def saved():
        call('session_save')
        return next(r for r in ET.parse(a.session_path / (a.session_path.name + '.ardour')).findall('.//Playlist/Region') if r.get('id') == region['regionId'])
    def shape(): return ET.tostring(saved().find('Extra/AutomationClipCurve'))
    cp('doublePress', .5, .7); cp('release', .5, .7)
    before = shape()
    assert len(saved().findall('Extra/AutomationClipCurve/Point')) == 3
    cp('press', .25, .35); cp('move', .25, .55, buttonDown=True); cp('release', .25, .55)
    bent = shape()
    assert bent != before and len(saved().find('Envelope/AutomationList/events').text.splitlines()) > 3
    call('session_undo'); assert shape() == before
    call('session_redo'); assert shape() == bent
    cp('press', .5, .7); cp('move', .6, .8, buttonDown=True); cp('release', .6, .8)
    moved = shape(); assert moved != bent
    cp('press', .6, .8); cp('move', .7, .6, buttonDown=True); key('Escape'); cp('release', .7, .6)
    assert shape() == moved
    cp('press', .6, .8); cp('release', .6, .8); key('BackSpace')
    assert len(saved().findall('Extra/AutomationClipCurve/Point')) == 2
    call('session_undo'); assert shape() == moved
    # Even a hidden Draw mode cannot add automation points in the arrangement.
    envelope = ET.tostring(saved().find('Envelope'))
    call('ui_activate_action', action='EditorEditing/set-mouse-mode-draw')
    box = clip_box()
    x, y = (box['x0'] + box['x1']) / 2, (box['y0'] + box['y1']) / 2
    pointer('EditorMainCanvas', 'press', x, y); pointer('EditorMainCanvas', 'release', x, y)
    assert shape() == moved and ET.tostring(saved().find('Envelope')) == envelope
    call('ui_activate_action', action='EditorEditing/set-mouse-mode-object')
    checks += ['double-click opens curve editor', 'add and bend points', 'compiled playback envelope', 'undo/redo', 'point move', 'Escape cancels', 'Delete edits point only', 'arrangement curve is read-only']
    remove_fixture([name], [route['id']])

elif a.case == 'scroll':
    names = ['UI Scroll Fixture ' + str(i) for i in range(6)]
    assert not any(r['name'] in names for r in routes()), 'Remove prior test fixtures first'
    ids = [create('Primary-t', n, 'audio_track') for n in names]
    pointer('EditorMainCanvas', 'scroll', 100, 100, deltaY=-1000)
    time.sleep(.08)
    c = next(c for c in main()['canvases'] if c['name'] == 'EditorMainCanvas')
    before = instant().attrib
    for delta in [3.25, 2.5, -1.0]: pointer(c['name'], 'scroll', 100, 100, deltaY=delta)
    wait(lambda: abs(float(instant().get('y-origin')) - float(before['y-origin']) - 4.75) < .01)
    checks += ['fractional pixel scroll accumulates']
    anchor = int(before['left-frame']) + 300 * int(before['zoom'])
    pointer(c['name'], 'pinch', 300, 100, magnification=.25)
    after = wait(lambda: (s if int(s['zoom']) < int(before['zoom']) else None) if (s := instant().attrib) else None)
    assert abs(int(after['left-frame']) + 300 * int(after['zoom']) - anchor) <= 2
    pointer(c['name'], 'pinch', 300, 100, magnification=-.2)
    after = instant().attrib
    assert abs(int(after['zoom']) - int(before['zoom'])) <= 2
    pointer(c['name'], 'scroll', 100, 100, deltaY=-4.75)
    checks += ['pinch anchors at event position', 'pinch reverses']
    remove_fixture(names, ids)

print(json.dumps(dict(case=a.case, passed=True, checks=checks, seconds=time.monotonic()-start), indent=2))
