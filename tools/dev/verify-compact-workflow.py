#!/usr/bin/env python3
"""Nine-second native GTK checks. Use a disposable session named *Verification*."""
import argparse, json, signal, subprocess, time, urllib.request
import xml.etree.ElementTree as ET
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--url', default='http://127.0.0.1:4838/mcp')
p.add_argument('--session-path', type=Path, required=True)
p.add_argument('--case', choices=['headers', 'track-title', 'search', 'scroll', 'native-scroll', 'curves', 'loop', 'master-clock', 'track-info', 'groups', 'group-drag', 'loop-locate', 'midi-clip-focus'], required=True)
p.add_argument('--native-scroll-helper', type=Path)
p.add_argument('--no-activate', action='store_true', help='Skip AppleEvents activation for native GTK-only checks on an isolated GUI')
a = p.parse_args()
if a.case == 'native-scroll':
    assert a.native_scroll_helper and a.native_scroll_helper.is_file(), 'Compile tests/native-scroll.swift and pass --native-scroll-helper'

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
    return result

def header(name, **kw):
    def ready():
        try: return pointer('TrackHeader:' + name, 'move', 30, 14).get('delivered')
        except AssertionError: return False
    wait(ready)  # Route creation/rename reaches GTK asynchronously.
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
    path = a.session_path / 'instant.xml'
    modified = path.stat().st_mtime_ns if path.exists() else 0
    call('ui_activate_action', action='Common/Save')
    def saved():
        if path.stat().st_mtime_ns == modified: return None
        try: return ET.parse(path).find('Editor')
        except ET.ParseError: return None
    return wait(saved)
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

def choose(dialog, label):
    # Dialog focus can start on either button depending on native activation.
    # Observe the actual label instead of assuming one Tab means confirmation.
    title = dialog['title']
    for _ in range(4):
        current = next(w for w in windows() if w['title'] == title)
        if current.get('focusText') == label:
            key('Return', title)
            return
        key('Tab', title)
        time.sleep(.02)
    raise AssertionError('Could not focus dialog button: ' + label)

def remove_fixture(names, ids):
    header(names[0])
    for name in names[1:]: header(name, primary=True)
    expect_selection(ids)
    key('BackSpace')
    dialog = wait(lambda: next((w for w in windows() if w['modal']), None))
    assert {r['id'] for r in routes()}.issuperset(ids)
    choose(dialog, 'Yes, remove them.' if len(ids) > 1 else 'Yes, remove it.')
    wait(lambda: not ({r['id'] for r in routes()} & set(ids)))

assert 'verification' in a.session_path.name.lower(), 'Use a disposable verification session'
assert Path(call('session_get_info')['sessionPath']).resolve() == a.session_path.resolve()
signal.signal(signal.SIGALRM, lambda *_: (_ for _ in ()).throw(TimeoutError('9-second test deadline')))
signal.alarm(9)
start = time.monotonic()
# Tests use GTK events; activation allows native dialogs/completion to own focus.
if not a.no_activate:
    port = a.url.split(':')[-1].split('/')[0]
    pid = subprocess.check_output(['lsof', '-t', '-iTCP:' + port, '-sTCP:LISTEN'], text=True).strip()
    subprocess.run(['osascript', '-e', 'tell application "System Events" to tell (first application process whose unix id is ' + pid + ') to set frontmost to true'], check=True)
header(next(r['name'] for r in routes() if not r['hidden'] and r['type'].endswith('_track')))
checks = []

if a.case == 'track-title':
    names = ['UI Title A', 'UI Title B', 'UI Title C']
    assert not any(r['name'].lower().startswith('ui title') for r in routes()), 'Remove old title fixtures before rerunning'
    # Insert the fixtures after the last visible scrolling route, before Master.
    for route in sorted((r for r in routes() if not r['hidden'] and r['name'] != 'Master'), key=lambda r: r['presentationOrder'], reverse=True):
        try: pointer('TrackHeader:' + route['name'], 'move', 30, 14)
        except AssertionError: continue
        header(route['name']); break
    ids = [create('Primary-t', name, 'audio_track') for name in names]
    def step(shortcut, route_id):
        key(shortcut); expect_selection([route_id])
    def rename_dialog(shortcut, title, original):
        key(shortcut)
        dialog = wait(lambda: next((w for w in windows() if w['title'] == title), None))
        assert dialog['textEntry'] and dialog['focusText'] == original, dialog
        return dialog
    def closed(title): wait(lambda: not any(w['title'] == title for w in windows()))
    first = min((r for r in routes() if not r['hidden'] and r['name'] != 'Master'), key=lambda r: r['presentationOrder'])
    header(first['name']); step('Up', first['id'])
    header(names[0]); step('Down', ids[1]); step('Up', ids[0])
    rename_dialog('Return', 'Rename Track', names[0])
    for shortcut in ['Left', 'Right', 'Up', 'Down']: key(shortcut, 'Rename Track')
    expect_selection(ids[:1])
    key('Escape', 'Rename Track'); closed('Rename Track')
    assert next(r for r in routes() if r['id'] == ids[0])['name'] == names[0]
    rename_dialog('KP_Enter', 'Rename Track', names[0])
    key('Primary-a', 'Rename Track')
    names[0] = 'ui title renamed'
    for ch in names[0]: key('space' if ch == ' ' else ch, 'Rename Track')
    key('Return', 'Rename Track'); closed('Rename Track')
    wait(lambda: next(r for r in routes() if r['id'] == ids[0])['name'] == names[0])
    step('Down', ids[1]); step('Up', ids[0])  # Rename restores header focus.
    checks += ['Return and keypad Enter open rename', 'entry arrows preserve track selection', 'Escape cancels', 'Enter commits and restores header navigation']

    def group(name):
        old = {r['id'] for r in routes()}; key('Primary-g')
        new = wait(lambda: [r for r in routes() if r['id'] not in old])
        assert len(new) == 1 and new[0]['type'] == 'bus', new
        call('track_rename', id=new[0]['id'], newName=name); header(name)
        return new[0]['id']
    def fold(name):
        for event in ['press', 'release']: pointer('TrackControl:' + name + '|group-fold', event, 8, 8)
        instant()  # Wait for queued redisplay before navigating.
    header(names[1], primary=True)
    inner = group('UI Title Inner')
    header(names[2], primary=True)
    outer = group('UI Title Outer')
    header('UI Title Outer')
    for route_id in [inner, *ids]: step('Down', route_id)
    for route_id in [ids[1], ids[0], inner, outer]: step('Up', route_id)
    fold('UI Title Inner'); header('UI Title Inner')
    step('Down', ids[2]); step('Up', inner)
    fold('UI Title Outer'); header('UI Title Outer')
    master = next(r['id'] for r in routes() if r['name'] == 'Master')
    step('Down', master); step('Down', master); step('Up', outer)
    rename_dialog('Return', 'Rename Bus', 'UI Title Outer')
    key('Primary-a', 'Rename Bus')
    for ch in 'ui title group': key('space' if ch == ' ' else ch, 'Rename Bus')
    key('Return', 'Rename Bus'); closed('Rename Bus')
    wait(lambda: next(r for r in routes() if r['id'] == outer)['name'] == 'ui title group')
    step('Down', master); step('Up', outer)
    fold('ui title group')
    step('Down', inner); step('Down', ids[2]); step('Up', inner)
    call('session_save')
    saved = {r.get('id'): r for r in ET.parse(a.session_path / (a.session_path.name + '.ardour')).findall('Routes/Route')}
    assert saved[inner].find('Extra/GroupTrack').get('parent') == outer
    for child in ids[:2]: assert saved[child].find('Extra/GroupTrack').get('parent') == inner
    assert saved[ids[2]].find('Extra/GroupTrack').get('parent') == outer
    checks += ['expanded nested groups navigate in display order both ways', 'collapsed descendants skipped', 'Master remains last without wrapping', 'collapsed group rename preserves hierarchy and inner fold state']
    # Keep the saved nested fixture for inspection in this disposable session.

elif a.case == 'groups':
    names = ['UI Group Child A', 'UI Group Child B', 'UI Group Child C']
    assert not any(r['name'].startswith('UI Group') for r in routes()), 'Remove old group fixtures before rerunning'
    ids = [create('Primary-t', name, 'audio_track') for name in names]
    def group(name):
        before = {r['id'] for r in routes()}
        key('Primary-g')
        created = wait(lambda: [r for r in routes() if r['id'] not in before])
        assert len(created) == 1 and created[0]['type'] == 'bus', created
        call('track_rename', id=created[0]['id'], newName=name)
        header(name)
        return created[0]['id']
    def saved_routes():
        call('session_save')
        return {r.get('id'): r for r in ET.parse(a.session_path / (a.session_path.name + '.ardour')).findall('Routes/Route')}
    def parent(route): return saved_routes()[route].find('Extra/GroupTrack').get('parent')
    def visible(name):
        try: return pointer('TrackHeader:' + name, 'move', 30, 14).get('delivered')
        except AssertionError: return False
    def fold(name):
        for event in ['press', 'release']: pointer('TrackControl:' + name + '|group-fold', event, 8, 8)
    def ungroup(name, group_id):
        header(name); key('Primary-Tertiary-g')
        dialog = wait(lambda: next((w for w in windows() if w['title'] == 'Ungroup tracks'), None))
        assert group_id in {r['id'] for r in routes()}
        choose(dialog, 'Ungroup')
        wait(lambda: group_id not in {r['id'] for r in routes()})
    header(names[0]); header(names[1], primary=True)
    inner = group('UI Group Inner')
    group_target = 'TrackControl:UI Group Inner|'
    for role in ['activator', 'solo', 'gain', 'pan']:
        pointer(group_target + role, 'move', 2, 2)
    try:
        pointer(group_target + 'record', 'move', 2, 2)
    except AssertionError as error:
        assert 'Invalid canvas' in str(error), error
    else:
        raise AssertionError('Group bus exposes record arm')
    assert parent(ids[0]) == inner and parent(ids[1]) == inner
    state = saved_routes()
    assert int(state[inner].find('PresentationInfo').get('order')) < int(state[ids[0]].find('PresentationInfo').get('order'))
    for child in ids[:2]:
        outputs = state[child].findall("IO[@direction='Output']/Port/Connection")
        assert len(outputs) == 2 and all('UI Group Inner/audio_in' in c.get('other') for c in outputs), [c.attrib for c in outputs]
    def name_geometry(name): return pointer('TrackControl:' + name + '|group-name', 'move', 2, 2)
    parent_box = name_geometry('UI Group Inner'); child_box = name_geometry(names[0])
    rail = pointer('TrackControl:' + names[0] + '|group-rail', 'move', 2, 2)
    assert child_box['xRoot'] == parent_box['xRoot'] + rail['width'], (parent_box, child_box, rail)
    assert child_box['xRoot'] + child_box['width'] == parent_box['xRoot'] + parent_box['width'], 'Routing columns shifted with child indentation'
    fold('UI Group Inner'); wait(lambda: not visible(names[0]) and not visible(names[1]))
    assert all(not r['hidden'] for r in routes() if r['id'] in ids)
    header('UI Group Inner'); header(names[2], primary=True)
    outer = group('UI Group Outer')
    assert parent(inner) == outer and parent(ids[2]) == outer
    fold('UI Group Outer'); wait(lambda: not visible('UI Group Inner') and not visible(names[2]))
    fold('UI Group Outer'); wait(lambda: visible('UI Group Inner') and visible(names[2]))
    assert not visible(names[0]), 'Inner fold state was lost'
    ungroup('UI Group Outer', outer)
    wait(lambda: visible('UI Group Inner') and visible(names[2]))
    assert not visible(names[0])
    ungroup('UI Group Inner', inner)
    wait(lambda: all(visible(name) for name in names))
    state = saved_routes()
    for child in ids[:2]:
        outputs = state[child].findall("IO[@direction='Output']/Port/Connection")
        assert len(outputs) == 2 and all('Master/audio_in' in c.get('other') for c in outputs), [c.attrib for c in outputs]
    header(names[0]); header(names[1], primary=True)
    doomed = group('UI Group Delete')
    fold('UI Group Delete'); wait(lambda: not visible(names[0]))
    key('BackSpace')
    dialog = wait(lambda: next((w for w in windows() if w['modal']), None))
    key('Escape', dialog['title'])
    wait(lambda: not any(w['modal'] for w in windows()))
    assert {doomed, *ids}.issubset({r['id'] for r in routes()})
    header('UI Group Delete')
    key('BackSpace')
    dialog = wait(lambda: next((w for w in windows() if w['modal']), None))
    choose(dialog, 'Yes, remove them.')
    wait(lambda: not ({doomed, *ids[:2]} & {r['id'] for r in routes()}))
    assert ids[2] in {r['id'] for r in routes()}
    remove_fixture(names[2:], ids[2:])
    checks += ['Cmd+G creates native group bus', 'exclusive child audio routing', 'parent-colored overhang keeps routing columns aligned', 'nested collapse preserves inner state', 'Cmd+Shift+G confirms and restores child outputs', 'cancelled deletion preserves folded children', 'confirmed deletion removes descendants only']

elif a.case == 'group-drag':
    names = ['UI Drag A', 'UI Drag B', 'UI Drag C']
    assert not any(r['name'].startswith('UI Drag') for r in routes()), 'Remove old drag fixtures before rerunning'
    ids = [create('Primary-t', name, 'audio_track') for name in names]
    def group(name):
        old = {r['id'] for r in routes()}
        key('Primary-g')
        new = wait(lambda: [r for r in routes() if r['id'] not in old])
        assert len(new) == 1, new
        call('track_rename', id=new[0]['id'], newName=name); header(name)
        return new[0]['id']
    def state():
        call('session_save')
        return {r.get('id'): r for r in ET.parse(a.session_path / (a.session_path.name + '.ardour')).findall('Routes/Route')}
    def parent(route):
        n = state()[route].find('Extra/GroupTrack')
        return n.get('parent', '') if n is not None else ''
    def routed(route, target):
        out = state()[route].findall("IO[@direction='Output']/Port/Connection")
        return len(out) == 2 and all(target + '/audio_in' in c.get('other') for c in out)
    def visible(name):
        try: return pointer('TrackHeader:' + name, 'move', 30, 14).get('delivered')
        except AssertionError: return False
    def fold(name):
        for event in ['press', 'release']: pointer('TrackControl:' + name + '|group-fold', event, 8, 8)
    def drag(source, target, fraction=.4, cancel=False):
        src = 'TrackHeader:' + source
        origin = pointer(src, 'move', 30, 14)
        dest = pointer('TrackHeader:' + target, 'move', 30, 14)
        x, y = dest['xRoot'] - origin['xRoot'] + 30, dest['yRoot'] - origin['yRoot'] + dest['height'] * fraction
        pointer(src, 'press', 30, 14)
        pointer(src, 'move', 30, 21, buttonDown=True)  # cross threshold before leaving source bounds
        pointer(src, 'move', x, y, buttonDown=True)
        if cancel:
            key('Escape')
            pointer(src, 'release', 30, 21)
        else:
            pointer(src, 'release', x, y)
    header(names[0]); inner = group('UI Drag Inner')
    fold('UI Drag Inner'); wait(lambda: not visible(names[0]))
    header(names[1]); drag(names[1], 'UI Drag Inner')
    wait(lambda: parent(ids[1]) == inner)
    assert routed(ids[1], 'UI Drag Inner') and not visible(names[1])
    expect_selection([inner])
    fold('UI Drag Inner'); wait(lambda: visible(names[1]))
    header(names[1]); drag(names[1], 'UI Drag Inner', .05)
    wait(lambda: parent(ids[1]) == '')
    assert routed(ids[1], 'Master')
    header(names[0]); header(names[1], primary=True)
    drag(names[1], 'UI Drag Inner')
    wait(lambda: parent(ids[1]) == inner)
    expect_selection(ids[:2])
    header('UI Drag Inner'); header(names[2], primary=True)
    outer = group('UI Drag Outer')
    header(names[0]); drag(names[0], 'UI Drag Outer')
    wait(lambda: parent(ids[0]) == outer)
    assert routed(ids[0], 'UI Drag Outer') and parent(ids[1]) == inner
    header('UI Drag Outer'); drag('UI Drag Outer', 'UI Drag Inner')
    dialog = wait(lambda: next((w for w in windows() if w['modal']), None))
    choose(dialog, 'gtk-ok'); wait(lambda: not any(w['modal'] for w in windows()))
    assert parent(outer) == '' and parent(inner) == outer
    header(names[2]); drag(names[2], 'UI Drag Outer', .05)
    wait(lambda: parent(ids[2]) == '')
    assert routed(ids[2], 'Master')
    header(names[2]); drag(names[2], 'UI Drag Inner', cancel=True)
    assert parent(ids[2]) == '' and routed(ids[2], 'Master')
    remove_fixture(['UI Drag Outer', names[2]], [outer, ids[2]])
    assert not ({inner, *ids} & {r['id'] for r in routes()})
    checks += ['native header drop into collapsed group', 'drop above group restores root routing', 'multi-selection survives drag', 'nested membership and routing', 'descendant cycle rejected', 'Escape cancels drop']

elif a.case == 'master-clock':
    call('transport_stop')
    wait(lambda: not call('session_get_info')['transport']['rolling'])
    def clock():
        if main().get('focusName') == 'LivePosition':
            key('Escape'); wait(lambda: not main()['textEntry'])
        call('ui_activate_action', action='Transport/focus-on-clock')
        return wait(lambda: (w if (w := main()).get('focusName') == 'LivePosition' else None))
    def enter(text):
        clock(); key('Primary-a')
        for ch in text: key('period' if ch == '.' else ch)
        wait(lambda: main().get('focusText') == text)
        key('Return')
    enter('22.1.1')
    rate = call('session_get_info')['sampleRate']
    bpm = call('session_get_info')['tempoBpm']
    # Verification fixture starts at 120 BPM, 4/4.
    expected = round(84 * 60 / bpm * rate)
    wait(lambda: call('session_get_info')['transport']['sample'] == expected)
    assert clock()['focusText'] == '22 . 1 . 1'
    key('Escape'); wait(lambda: not main()['textEntry'])
    enter('3.2.3'); expected = round(9.5 * 60 / bpm * rate)
    wait(lambda: call('session_get_info')['transport']['sample'] == expected)
    assert clock()['focusText'] == '3 . 2 . 3'
    enter('3.9.1'); assert call('session_get_info')['transport']['sample'] == expected
    key('Escape'); wait(lambda: not main()['textEntry'])
    checks += ['compact bar.beat.sixteenth input/display', 'invalid beat rejected', 'Escape releases keyboard focus']
    master = next(r for r in routes() if r['name'] == 'Master')
    header('Master'); expect_selection([master['id']])
    def geometry(): return pointer('TrackHeader:Master', 'move', 30, 14)
    top = geometry()
    pointer('EditorMainCanvas', 'scroll', 100, 100, deltaY=350)
    wait(lambda: float(instant().get('y-origin', '0')) > 0)
    bottom = geometry()
    assert top['yRoot'] == bottom['yRoot'] and top['height'] == bottom['height'], (top, bottom)
    c = next(c for c in main()['canvases'] if c['name'] == 'EditorMainCanvas')
    assert abs(top['yRoot'] + top['height'] - c['yRoot'] - c['height']) <= 2, (top, c)
    # Hit the canvas side of the pinned row; it must select Master, not a
    # scrolling track underneath it. A click leaves the session's music intact.
    call('ui_activate_action', action='Common/deselect-all')
    expect_selection([])
    time.sleep(.1)  # Bottom device pane changes allocation with track selection.
    def master_y():
        h = geometry(); canvas = next(c for c in main()['canvases'] if c['name'] == 'EditorMainCanvas')
        return h['yRoot'] - canvas['yRoot'] + h['height'] / 2
    y = master_y()
    for e in ['move', 'press']: pointer('EditorMainCanvas', e, 200, y)
    expect_selection([master['id']])
    time.sleep(.1)
    pointer('EditorMainCanvas', 'release', 200, master_y())
    pointer('EditorMainCanvas', 'scroll', 100, 100, deltaY=-1000)
    wait(lambda: float(instant().get('y-origin', '0')) == 0)
    final = geometry(); canvas = next(c for c in main()['canvases'] if c['name'] == 'EditorMainCanvas')
    assert abs(final['yRoot'] + final['height'] - canvas['yRoot'] - canvas['height']) <= 2
    checks += ['native Master header fixed while scrolling', 'Master canvas/header aligned', 'canvas hit selects Master over scrolling tracks']

elif a.case == 'track-info':
    name = 'UI Header Detail Test'
    assert not any(r['name'] == name for r in routes())
    route_id = create('Primary-t', name, 'audio_track')
    header(name)
    def control(role, button=1):
        for event in ['press', 'release']:
            pointer('TrackControl:' + name + '|' + role, event, 10, 10, button=button)
    def monitor_value():
        instant()
        root = ET.parse(a.session_path / (a.session_path.name + '.ardour'))
        return root.find(".//Route[@id='" + route_id + "']/Controllable[@name='monitor']").get('value')
    for role, value in [('monitor-in', '1'), ('monitor-off', '2'), ('monitor-auto', '0')]:
        control(role); wait(lambda: monitor_value() == value)
    def info(): return call('track_get_info', id=route_id)
    for role, field in [('activator', 'mute'), ('solo', 'solo'), ('record', 'recEnabled')]:
        initial = info()[field]
        control(role)
        wait(lambda: info()[field] != initial)
        control(role)
        wait(lambda: info()[field] == initial)
    checks += ['number toggles native mute, solo and record arm toggle independently']
    target = 'TrackControl:' + name + '|'
    def geometry(role): return pointer(target + role, 'move', 2, 2)
    activator, solo, record = map(geometry, ['activator', 'solo', 'record'])
    assert activator['xRoot'] < solo['xRoot'] < record['xRoot']
    assert activator['yRoot'] == solo['yRoot'] == record['yRoot']
    gain, pan = map(geometry, ['gain', 'pan'])
    assert gain['yRoot'] > activator['yRoot'] and gain['xRoot'] < pan['xRoot']
    assert abs(gain['yRoot'] - pan['yRoot']) <= 2
    def drag(role):
        pointer(target + role, 'press', 8, 8)
        pointer(target + role, 'move', 22, 8, down=True)
        pointer(target + role, 'release', 22, 8)
    before = info()['pan']['position']
    drag('pan')
    wait(lambda: abs(info()['pan']['position'] - before) > 0.01)
    for event in ['press', 'release']:
        pointer(target + 'pan', event, 8, 8, shift=True)
    wait(lambda: abs(info()['pan']['position'] - 0.5) < 0.001)
    before = info()['fader']['db']
    drag('gain')
    wait(lambda: abs(info()['fader']['db'] - before) > 0.1)
    pointer(target + 'gain', 'doublePress', 10, 8)
    wait(lambda: main()['textEntry'])
    key('Primary-a'); key('minus'); key('6'); key('Return')
    wait(lambda: abs(call('track_get_info', id=route_id)['fader']['db'] + 6) < .02)
    assert not call('session_get_info')['transport']['rolling']
    checks += ['In/Auto/Off writes native monitor control', 'gain and pan drag, double-click gain for numeric entry']
    for role in ['input', 'output']:
        control(role, button=3)
        title = name + ' ' + role
        wait(lambda: any(w['title'] == title for w in windows()))
        call('ui_close_dialog', window=title)
        wait(lambda: not any(w['title'] == title for w in windows()))
    checks += ['input and output open native routing editors']
    remove_fixture([name], [route_id])

elif a.case == 'headers':
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

elif a.case == 'loop-locate':
    call('transport_stop'); wait(lambda: not call('transport_get_state')['rolling'])
    master = next(r for r in routes() if r['name'] == 'Master')
    call('track_set_mute', id=master['id'], value=True)  # Quiet disposable fixture.
    call('transport_loop_toggle', enabled=False)
    name = 'UI Loop Locate'
    assert not any(r['name'] == name for r in routes()), 'Remove prior loop fixture'
    route_id = create('Primary-t', name, 'audio_track'); header(name)
    call('ui_activate_action', action='EditorEditing/set-mouse-mode-object')
    call('ui_activate_action', action='EditorEditing/snap-normal')
    call('ui_activate_action', action='Editor/zoom-to-session')
    pointer('EditorMainCanvas', 'scroll', 100, 100, deltaY=-1000, deltaX=-1000)
    time.sleep(.1)
    state = instant().attrib
    zoom, left = int(state['zoom']), int(state['left-frame'])
    info = call('session_get_info'); rate = info['sampleRate']
    bar = int(rate * 60 / info['tempoBpm'] * 4)
    assert state['grid-type'] == 'GridTypeBar', state
    call('markers_set_auto_loop_samples', startSample=2*bar, endSample=7*bar)
    call('transport_loop_toggle', enabled=True)
    call('transport_play'); wait(lambda: call('transport_get_state')['rolling'])
    def click_bar(number, rolling=True):
        c = next(c for c in main()['canvases'] if c['name'] == 'EditorMainCanvas')
        h = pointer('TrackHeader:' + name, 'move', 30, 14)
        x = ((number-1+.08)*bar-left)/zoom
        y = h['yRoot'] - c['yRoot'] + 24
        assert 0 < x < c['width'] and 0 < y < c['height'], (x,y,c,h)
        for event in ['move','press','release']: pointer('EditorMainCanvas',event,x,y)
        target = (number-1)*bar
        def located():
            t = call('transport_get_state')
            return t['rolling'] == rolling and target <= t['sample'] < target + (rate//4 if rolling else 1)
        wait(located)
    click_bar(5)
    click_bar(2)  # Before the bracket: play here, then encounter the loop normally.
    click_bar(9)  # After it: do not pull playback back to the bracket.
    call('transport_locate_sample', sample=7*bar-rate//5)
    wait(lambda: 7*bar-rate//2 < call('transport_get_state')['sample'] < 7*bar)
    wait(lambda: 2*bar <= call('transport_get_state')['sample'] < 2*bar+rate//2)
    loop = next(m for m in call('markers_list')['markers'] if m['isAutoLoop'])
    assert (loop['startSample'],loop['endSample']) == (2*bar,7*bar)
    call('transport_stop'); wait(lambda: not call('transport_get_state')['rolling'])
    click_bar(5, rolling=False)
    call('transport_loop_toggle', enabled=False)
    remove_fixture([name],[route_id])
    checks += ['arrangement click plays at snapped point while looping', 'clicks before/after bracket are honored', 'loop bounds and wrapping preserved', 'stopped click does not start playback']

elif a.case == 'midi-clip-focus':
    midi = next(r for r in routes() if r['type'] == 'midi_track' and not r['hidden'])
    def regions():
        return call('track_get_regions', id=midi['id'])['regions']
    original = {r['regionId'] for r in regions()}
    state = instant().attrib
    info = call('session_get_info')
    bar = round(info['sampleRate'] * 60 / info['tempoBpm'] * 4)
    zoom, left = int(state['zoom']), int(state['left-frame'])
    canvas = next(c for c in main()['canvases'] if c['name'] == 'EditorMainCanvas')
    def select_bars(route, start, end):
        box = pointer('TrackHeader:' + route['name'], 'move', 20, 12)
        y = box['yRoot'] - canvas['yRoot'] + box['height'] / 2
        x1, x2 = ((b * bar - left) / zoom for b in (start + .05, end + .05))
        assert 0 < x1 < x2 < canvas['width'] and 0 < y < canvas['height']
        pointer('EditorMainCanvas', 'move', x1, y)
        pointer('EditorMainCanvas', 'press', x1, y)
        pointer('EditorMainCanvas', 'move', x2, y, buttonDown=True)
        pointer('EditorMainCanvas', 'release', x2, y)
        assert instant().find('Selection/TimelineRange') is not None
    def new_regions():
        return [r for r in regions() if r['regionId'] not in original]
    key('Escape')
    call('ui_activate_action', action='EditorEditing/set-mouse-mode-object')
    call('ui_activate_action', action='EditorEditing/snap-normal')
    select_bars(midi, 2, 4)
    key('Primary-Tertiary-m')
    clip, = wait(new_regions)
    assert (clip['startSample'], clip['endSample']) == (2 * bar, 4 * bar), clip
    assert not call('midi_note_get_json', regionId=clip['regionId'])['midi']['midi_events']
    key('Primary-Tertiary-m')
    assert len(new_regions()) == 1
    call('session_undo')
    wait(lambda: not new_regions())
    call('session_redo')
    wait(lambda: len(new_regions()) == 1)
    select_bars(midi, 4, 5)
    key('Primary-Tertiary-m')
    wait(lambda: len(new_regions()) == 2)
    key('Escape')
    key('Down')
    following = selected()
    assert following and following != {midi['id']}
    key('Escape')
    assert not selected()
    key('Up')
    expect_selection([midi['id']])
    key('Tab')
    key('Down')
    expect_selection(following)
    key('Up')
    expect_selection([midi['id']])
    call('transport_locate_sample', sample=2 * bar)
    wait(lambda: call('transport_get_state')['sample'] == 2 * bar)
    key('Right')
    wait(lambda: call('transport_get_state')['sample'] > 2 * bar)
    key('Left')
    wait(lambda: call('transport_get_state')['sample'] == 2 * bar)
    expect_selection([midi['id']])
    call('session_undo')
    call('session_undo')
    wait(lambda: not new_regions())
    call('ui_activate_action', action='EditorEditing/snap-off')
    select_bars(midi, .25, 1.25)
    off_grid = instant().find('Selection/TimelineRange').attrib
    call('ui_activate_action', action='EditorEditing/snap-normal')
    key('Primary-Tertiary-m')
    off_grid_clip, = wait(new_regions)
    call('session_save')
    session_xml = ET.parse(a.session_path / (a.session_path.name + '.ardour'))
    saved = session_xml.find(".//Playlist/Region[@id='" + off_grid_clip['regionId'] + "']")
    assert saved is not None
    length, position = saved.get('length').split('@')
    assert position == off_grid['start']
    assert off_grid['start'][0] == off_grid['end'][0] == length[0] == 'b'
    assert int(off_grid['end'][1:]) - int(off_grid['start'][1:]) == int(length[1:])
    assert off_grid_clip['startSample'] % bar and off_grid_clip['endSample'] % bar
    call('session_undo')
    wait(lambda: not new_regions())
    audio = next(r for r in routes() if r['type'] == 'audio_track' and not r['hidden'])
    before_audio = call('track_get_regions', id=audio['id'])['regions']
    select_bars(audio, 2, 4)
    key('Primary-Tertiary-m')
    assert call('track_get_regions', id=audio['id'])['regions'] == before_audio and not new_regions()
    checks += ['empty MIDI clip matches dragged range', 'repeat skips occupied range',
               'undo and redo', 'adjacent MIDI insertion', 'vertical navigation resumes after Escape and from the device list',
               'horizontal arrows move the playhead and retain track focus',
               'off-grid selection survives snap changes', 'audio range is skipped']

elif a.case == 'loop':
    call('transport_stop')
    wait(lambda: not call('session_get_info')['transport']['rolling'])
    call('transport_loop_toggle', enabled=False)
    call('ui_activate_action', action='EditorEditing/set-mouse-mode-object')
    call('ui_activate_action', action='EditorEditing/snap-normal')
    call('ui_activate_action', action='Editor/zoom-to-session')
    pointer('EditorMainCanvas', 'scroll', 100, 100, deltaY=-1000, deltaX=-1000)
    time.sleep(.1)  # Zoom/layout changes apply on the next GTK redraw.
    state = instant().attrib
    zoom, left = int(state['zoom']), int(state['left-frame'])
    info = call('session_get_info')
    bar = int(info['sampleRate'] * 60 / info['tempoBpm'] * 4)
    assert left == 0 and state['grid-type'] == 'GridTypeBar', state
    first = min((r for r in routes() if r['type'].endswith('_track') and not r['hidden']), key=lambda r: r['presentationOrder'])
    regions = call('track_get_regions', id=first['id'])['regions']
    assert regions and all(r['endSample'] < 2 * bar for r in regions), 'Use the compact verification fixture with empty bars 3–9'
    def canvas(): return next(c for c in main()['canvases'] if c['name'] == 'EditorMainCanvas')
    c = canvas()
    label = next(l for l in c['labels'] if regions[0]['name'] in l['text'])
    box = label['ancestors'][0]
    y = (box['y0'] + box['y1']) / 2
    def x(b): return ((b-1) * bar - left) / zoom
    assert 0 < y < c['height'] and x(9) < c['width'], c
    def drag(x1, y1, x2, y2, cancel=False):
        pointer('EditorMainCanvas', 'move', x1, y1)
        pointer('EditorMainCanvas', 'press', x1, y1)
        # One coalesced motion must still anchor at the original press.
        pointer('EditorMainCanvas', 'move', x2, y2, buttonDown=True)
        if cancel: key('Escape')
        pointer('EditorMainCanvas', 'release', x2, y2)
    def loop(): return next((m for m in call('markers_list')['markers'] if m['isAutoLoop']), None)
    def bounds():
        m = loop()
        return (m['startSample'], m['endSample']) if m else None
    def expect(b1, b2): wait(lambda: bounds() == ((b1-1)*bar, (b2-1)*bar))
    before = call('session_get_info')['transport']['sample']
    drag(x(3.12), y, x(5.14), y)
    assert call('session_get_info')['transport']['sample'] == before
    assert first['id'] in selected()  # Native linked track groups may join the selection.
    time_selection = instant().find('Selection')
    assert len(list(time_selection)), ET.tostring(time_selection)
    key('Primary-l'); expect(3, 5)
    time.sleep(.1)  # Allow the audio thread to process SetLoop.
    assert not call('session_get_info')['transport']['rolling']
    call('session_save')
    saved = ET.parse(a.session_path / (a.session_path.name + '.ardour'))
    ruler = saved.find('.//RulerVisibility')
    assert ruler.get('bbt') == '1' and ruler.get('rangemarker') == '1', ruler.attrib
    start_marker = next(l for l in canvas()['labels'] if l['text'] == 'start')
    bracket_y = start_marker['y0'] + 5
    drag(x(4), bracket_y, x(5), bracket_y); expect(4, 6)
    drag(x(6)-2, bracket_y, x(7)-2, bracket_y); expect(4, 7)
    drag(x(4)+2, bracket_y, x(5)+2, bracket_y); expect(5, 7)
    call('session_undo'); expect(4, 7)
    call('session_redo'); expect(5, 7)
    drag(x(6), bracket_y, x(7), bracket_y, cancel=True); expect(5, 7)
    checks += ['empty bars select from original press', 'Cmd+L works without regions and keeps transport stopped', 'bar and loop rulers default on', 'bracket moves both ends', 'end grips resize independently', 'loop undo/redo', 'Escape cancels bracket drag']
    # Zoom refreshes the bracket width as well as its handles.
    pointer('EditorMainCanvas', 'pinch', 0, y, magnification=-.2)
    state = instant().attrib
    zoom, left = int(state['zoom']), int(state['left-frame'])
    drag(x(6), bracket_y, x(5), bracket_y); expect(4, 6)
    # A reversed empty-space drag must also produce an ordered snapped range.
    drag(x(8.1), y, x(3.1), y)
    key('Primary-l'); expect(3, 8)
    pointer('EditorMainCanvas', 'move', x(9.1), y)
    pointer('EditorMainCanvas', 'press', x(9.1), y)
    pointer('EditorMainCanvas', 'release', x(9.1), y)
    wait(lambda: call('session_get_info')['transport']['sample'] == 8 * bar)
    assert call('track_get_regions', id=first['id'])['regions'] == regions
    call('session_save')
    checks += ['bracket remains draggable after zoom', 'reverse selection snaps', 'plain click still locates', 'regions unchanged', 'loop saved']
    # Cross the boundary twice, with a Stop/Play between, rather than waiting
    # for a whole song. This checks actual transport looping, not just drawing.
    for _ in range(2):
        call('transport_play')
        wait(lambda: call('session_get_info')['transport']['rolling'])
        call('transport_locate_sample', sample=7 * bar - info['sampleRate'] // 5)
        wait(lambda: call('session_get_info')['transport']['sample'] > 6 * bar)
        wait(lambda: 2 * bar <= call('session_get_info')['transport']['sample'] < 3 * bar)
        call('transport_stop')
        wait(lambda: not call('session_get_info')['transport']['rolling'])
    checks += ['audio transport actually wraps', 'Stop/Play retains loop mode']
    def cross_boundary(enabled, start_playback=True):
        lo, hi = bounds()
        if start_playback: call('transport_play')
        wait(lambda: call('session_get_info')['transport']['rolling'])
        call('transport_locate_sample', sample=hi - info['sampleRate'] // 5)
        wait(lambda: hi - info['sampleRate'] // 2 < call('session_get_info')['transport']['sample'] < hi)
        if enabled:
            wait(lambda: lo <= call('session_get_info')['transport']['sample'] < lo + bar)
        else:
            wait(lambda: call('session_get_info')['transport']['sample'] > hi + 1024)
        wait(lambda: call('session_get_info')['transport']['rolling'])
    # Same highlighted range toggles off/on while playback continues.
    drag(x(3.12), y, x(8.14), y)
    call('transport_play')
    wait(lambda: call('session_get_info')['transport']['rolling'])
    key('Primary-l'); time.sleep(.1)
    wait(lambda: call('session_get_info')['transport']['rolling'])
    expect(3, 8); cross_boundary(False, start_playback=False)
    key('Primary-l'); time.sleep(.1)
    wait(lambda: call('session_get_info')['transport']['rolling'])
    expect(3, 8); cross_boundary(True, start_playback=False)
    call('transport_stop')
    wait(lambda: not call('session_get_info')['transport']['rolling'])
    # No selected clips or range toggles the existing loop without moving it.
    call('ui_activate_action', action='Common/deselect-all')
    wait(lambda: len(list(instant().find('Selection'))) == 0)
    key('Primary-l'); time.sleep(.1)
    assert not call('session_get_info')['transport']['rolling']
    expect(3, 8); cross_boundary(False)
    call('transport_stop')
    wait(lambda: not call('session_get_info')['transport']['rolling'])
    key('Primary-l'); time.sleep(.1)
    assert not call('session_get_info')['transport']['rolling']
    expect(3, 8); cross_boundary(True)
    call('transport_stop')
    wait(lambda: not call('session_get_info')['transport']['rolling'])
    checks += ['same selection toggles off/on without stopping playback', 'no selection toggles off/on without starting playback', 'disabled loop plays past end', 'toggle preserves loop bounds']


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

elif a.case in ('scroll', 'native-scroll'):
    names = ['UI Scroll Fixture ' + str(i) for i in range(6)]
    assert not any(r['name'] in names for r in routes()), 'Remove prior test fixtures first'
    ids = [create('Primary-t', n, 'audio_track') for n in names]
    pointer('EditorMainCanvas', 'scroll', 100, 100, deltaY=-1000)
    time.sleep(.08)
    c = next(c for c in main()['canvases'] if c['name'] == 'EditorMainCanvas')
    before = wait(lambda: (state.attrib if float(state.get('y-origin')) == 0 else None) if (state := instant()) is not None else None)
    for delta in [3.25, 2.5, -1.0]: pointer(c['name'], 'scroll', 100, 100, deltaY=delta)
    wait(lambda: abs(float(instant().get('y-origin')) - float(before['y-origin']) - 4.75) < .01)
    checks += ['fractional pixel scroll accumulates']
    if a.case == 'native-scroll':
        # CGEvent -> Cocoa -> Quartz GDK -> editor (no MCP event bypass).
        def native(dx, dy, mode='gesture', over_header=False):
            x, y = c['xRoot'] + 100, c['yRoot'] + 100
            if over_header:
                name = next(r['name'] for r in routes() if not r['hidden'] and r['type'].endswith('_track'))
                h = pointer('TrackHeader:' + name, 'move', 30, 14)
                x, y = h['xRoot'] + 30, h['yRoot'] + h['height'] / 2
            subprocess.run([str(a.native_scroll_helper), pid, str(x), str(y), str(dx), str(dy), mode], check=True, timeout=1)
        baseline = float(instant().get('y-origin'))
        expected = baseline
        for delta in [-3, -4, 2]:
            native(0, delta)
            expected -= delta
            wait(lambda: abs(float(instant().get('y-origin')) - expected) < .01)
        native(0, -2, 'momentum')
        expected += 2
        wait(lambda: abs(float(instant().get('y-origin')) - expected) < .01)
        native(0, -3, over_header=True)
        expected += 3
        wait(lambda: abs(float(instant().get('y-origin')) - expected) < .01)
        old_x = int(instant().get('left-frame'))
        native(-2, 0)
        wait(lambda: int(instant().get('left-frame')) == old_x + 2 * int(before['zoom']))
        native(2, 0)
        wait(lambda: int(instant().get('left-frame')) == old_x)
        native(0, -1, 'wheel')
        expected += 40
        wait(lambda: abs(float(instant().get('y-origin')) - expected) < .01)
        native(0, 1, 'wheel')
        expected -= 40
        wait(lambda: abs(float(instant().get('y-origin')) - expected) < .01)
        pointer(c['name'], 'scroll', 100, 100, deltaY=baseline - expected)
        checks += ['native pixel increments and reversal', 'native momentum', 'native track-header scrolling', 'native horizontal pixel movement', 'legacy wheel uses pixels, not track boundaries']
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
