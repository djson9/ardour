#!/usr/bin/env python3
"""Measure arrow-key handling plus a GUI-thread MCP inspection round trip.

Run against a stopped session, with editor focus (not a text field).
This changes track selection. It does not measure compositor presentation.
For cold-selection measurements, run immediately after restarting Ardour.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import statistics
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--start-track-id', required=True, help='Track immediately before the target')
parser.add_argument('--window', required=True, help='Exact editor window title')
parser.add_argument('--iterations', type=int, default=10)
parser.add_argument('--budget-ms', type=float, default=100)
args = parser.parse_args()
if args.iterations < 1:
    parser.error('--iterations must be positive')
spec = importlib.util.spec_from_file_location('iterate', Path(__file__).with_name('macos-fast-iterate.py'))
api = importlib.util.module_from_spec(spec)
spec.loader.exec_module(api)
api.tool('track_select', id=args.start_track_id)
state = api.tool('ui_get_state')
window = next((w for w in state['windows'] if w['title'] == args.window), None)
if not window or window['textEntry'] or window['modal']:
    raise SystemExit('Editor window must be visible with non-text, non-modal focus')
for action, key in [('Editor/select-next-route', 'Down'), ('Editor/select-prev-route', 'Up')]:
    state = api.tool('ui_get_state', action=action, group='Editor')
    if not state['actionExists'] or state['key'] != key:
        raise SystemExit(f'Expected {key} binding for {action}: {state}')
rows = []
time.sleep(.2)
for _ in range(args.iterations):
    for key in ['Down', 'Up']:
        start = time.perf_counter()
        api.tool('ui_press_key', key=key, window=args.window)
        api.tool('ui_get_state')  # waits behind the queued key on the GUI thread
        rows.append({'key': key, 'roundtrip_ms': round((time.perf_counter() - start) * 1000, 3)})
        time.sleep(.08)
summary = {}
for key in ['Down', 'Up']:
    samples = [r['roundtrip_ms'] for r in rows if r['key'] == key]
    summary[key] = {'first_ms': samples[0], 'median_ms': statistics.median(samples), 'max_ms': max(samples)}
print(json.dumps({'metric': 'key_dispatch_and_gui_inspection_roundtrip_ms', 'summary': summary, 'samples': rows}, indent=2))
raise SystemExit(0 if max(r['roundtrip_ms'] for r in rows) < args.budget_ms else 1)
