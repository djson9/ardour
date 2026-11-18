#!/usr/bin/env python3
"""Run the native bottom-panel rack regression in an isolated macOS GUI job.
Requires a built app, dummy backend, and passing verify-audio-racks.py tests.
"""
import argparse
import json
import os
import plistlib
from pathlib import Path
import subprocess
import sys
import time
import urllib.request
import xml.etree.ElementTree as ET

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--floating', action='store_true', help='Exercise the optional floating editor instead')
parser.add_argument('--drag', action='store_true', help='Exercise native mouse drag and drop (embedded panel only)')
parser.add_argument('--prepared', action='store_true', help='Use an already prepared isolated GUI fixture and launch plist')
parser.add_argument('--port', type=int, default=4838, help='Isolated fixture MCP port')
args = parser.parse_args()
assert not (args.drag and args.floating)
root = Path(__file__).resolve().parents[2]
out = root / 'artifacts/audio-racks'
service = f'gui/{os.getuid()}/dev.ardour.audio-racks-verification'
if subprocess.run(['launchctl', 'print', service], capture_output=True).returncode == 0:
    raise SystemExit('The rack verification job is already loaded; leaving it alone.')
if not args.prepared:
    assert args.port == 4838, 'Custom ports require a prepared fixture'
    subprocess.run([sys.executable, str(root / 'tools/dev/verify-audio-racks.py'), '--prepare-gui'] + ([] if args.floating else ['--embedded']), check=True)
if args.drag:
    plist = out / 'gui.plist'
    job = plistlib.loads(plist.read_bytes())
    job['EnvironmentVariables']['ARDOUR_RACK_UI_DRAG'] = '1'
    plist.write_bytes(plistlib.dumps(job))
    for name in ('drag-request.json', 'drag-done'):
        (out / name).unlink(missing_ok=True)
    subprocess.run(['swiftc', str(root / 'tools/dev/rack-drag-mouse.swift'), '-o', str(out / 'drag-mouse')], check=True)
fixture = out / 'Rack Verification'
session_file = fixture / 'Rack Verification.ardour'
route_id = next(r.get('id') for r in ET.parse(session_file).findall('./Routes/Route') if r.find("Processor[@type='audio-rack']") is not None)
initial_plugin_id = ET.parse(session_file).find(f"./Routes/Route[@id='{route_id}']/Processor[@type='audio-rack']/Chain/Processor").get('id')
log = out / 'gui.log'
offset = log.stat().st_size if log.exists() and not args.prepared else 0

def call(tool_name, **arguments):
    data = json.dumps(dict(jsonrpc='2.0', id=1, method='tools/call', params=dict(name=tool_name, arguments=arguments))).encode()
    request = urllib.request.Request(f'http://127.0.0.1:{args.port}/mcp', data, {'Content-Type': 'application/json'})
    result = json.load(urllib.request.urlopen(request, timeout=3))['result']
    if result.get('isError'):
        raise RuntimeError(result)
    return result.get('structuredContent', result)

subprocess.run(['launchctl', 'bootstrap', f'gui/{os.getuid()}', str(out / 'gui.plist')], check=True)
try:
    deadline = time.monotonic() + 125
    last_drag = 0
    selected = False
    navigation = 0
    while time.monotonic() < deadline:
        if not selected:
            try:
                info = call('session_get_info')
            except OSError:
                time.sleep(.2)
                continue
            assert info['sessionPath'] == str(fixture), info
            call('track_select', id=route_id)
            selected = True
        with log.open('rb') as stream:
            stream.seek(offset)
            text = stream.read().decode(errors='replace')
        request = out / 'drag-request.json'
        if args.drag and request.exists():
            drag = json.loads(request.read_text())
            if drag['id'] > last_drag:
                assert call('session_get_info')['sessionPath'] == str(fixture)
                subprocess.run([str(out / 'drag-mouse'), *map(str, drag['from']), *map(str, drag['to']), str(drag['cancel']), subprocess.check_output(['lsof', '-t', f'-iTCP:{args.port}', '-sTCP:LISTEN'], text=True).strip()], check=True, timeout=8)
                time.sleep(.8)
                last_drag = drag['id']
                (out / 'drag-done').write_text(str(last_drag))
        if 'RACK PROBE FAILED:' in text:
            raise RuntimeError(text[text.index('RACK PROBE FAILED:'):])
        if navigation == 0 and 'RACK PROBE READY: select empty track' in text:
            call('tracks_add', type='audio', count=1, name='Rack navigation', inputChannels=2, outputChannels=2)
            empty = next(t for t in call('tracks_list')['tracks'] if t['name'] == 'Rack navigation')
            call('track_select', id=empty['id'])
            navigation = 1
        if navigation == 1 and 'RACK PROBE READY: return to rack track' in text:
            call('track_select', id=route_id)
            navigation = 2
        if ('RACK PROBE PASSED: floating rack' if args.floating else 'RACK PROBE PASSED: bottom panel') in text:
            assert args.floating or navigation == 2
            assert not args.drag or (last_drag == 5 and 'RACK PROBE PASSED: native mouse drags' in text)
            assert call('session_save')['saved']
            rack = ET.parse(session_file).find(f"./Routes/Route[@id='{route_id}']/Processor[@type='audio-rack']")
            names = [[p.get('name') for p in c.findall('Processor')] for c in rack.findall('Chain')]
            assert names == [['Rack test'], ['ACE EQ']], names
            if args.drag:
                assert rack.find('Chain/Processor').get('id') == initial_plugin_id
                assert not rack.findall('Macro/Mapping')
                assert len(ET.parse(session_file).findall(f".//Processor[@id='{initial_plugin_id}']")) == 1
                assert rack.find('Chain/Processor').find('.//LearnedParameters/Parameter') is not None
            result = {'native_mouse_drag': args.drag, 'drag_identity_and_mapping_checks': args.drag, 'bottom_panel': not args.floating, 'floating_editor': args.floating, 'selected_chain_lists': True, 'insertion_reorder_removal': True,
                      'track_switch_lifecycle': navigation == 2, 'saved_chain_plugins': names}
            (out / ('drag-result.json' if args.drag else ('floating-panel-result.json' if args.floating else 'bottom-panel-result.json'))).write_text(json.dumps(result, indent=2) + '\n')
            print(json.dumps(result, indent=2))
            break
        time.sleep(.2)
    else:
        raise RuntimeError('Bottom-panel verification timed out; inspect artifacts/audio-racks/gui.log')
finally:
    subprocess.run(['launchctl', 'bootout', service], check=True)
