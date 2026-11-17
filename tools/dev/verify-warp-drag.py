#!/usr/bin/env python3
"""Exercise the actual Shift-edge gesture in a disposable Warp Verification session."""
import argparse,importlib.util,json,time
from pathlib import Path
spec=importlib.util.spec_from_file_location('m',Path(__file__).with_name('macos-fast-iterate.py'));m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
ap=argparse.ArgumentParser();ap.add_argument('--hold-seconds',type=float,default=0);ap.add_argument('--loop',action='store_true');ap.add_argument('--cancel',action='store_true');ap.add_argument('--channels',type=int,default=1);ap.add_argument('--keep-commit',action='store_true');ap.add_argument('--left-edge',action='store_true');ap.add_argument('--beat-grid',action='store_true');args=ap.parse_args()
initial_session=m.tool('session_get_info');assert initial_session['sessionName']=='Warp Verification'
engine_samples=[]
session_path=Path(initial_session['sessionPath']);files_before=set(session_path.rglob('*.wav'))
def state():return next(w for w in m.tool('ui_get_state')['windows'] if w['title'].endswith(' - Ardour'))
def metrics():return m.tool('track_get_info',id='488')['warpPreview']
if state()['canvases'][0]['height']<300:
    m.tool('ui_activate_action',action='Editor/show-editor-props');time.sleep(.15)
a=state();canvas=next(c for c in a['canvases'] if c['name']=='EditorMainCanvas');label=next(t for t in canvas['labels'] if t['text']=='Warp Fixture');box=label['ancestors'][0]
x=box['x0']+5 if args.left_edge else box['x1']-5
y=box['y1']-20;direction=1 if args.left_edge else -1;target=x+direction*90
if args.beat_grid:
    m.tool('ui_activate_action',action='EditorSnap/grid-type-beat');m.tool('ui_activate_action',action='EditorEditing/snap-normal');time.sleep(.1)
assert 0<x<canvas['width'] and 0<y<canvas['height'],(canvas,box)
def pointer(event,pos,down):
    current=state();c=next(c for c in current['canvases'] if c['name']=='EditorMainCanvas')
    event_y=min(y,c['height']-1) if event=='release' and not c['grabbed'] else y
    return m.tool('ui_canvas_pointer',window=current['title'],canvas='EditorMainCanvas',event=event,x=pos,y=event_y,shift=True,buttonDown=down)
before=m.tool('track_get_regions',id='488')['regions'][0];baseline=metrics();initial_metrics=baseline.copy();samples=[]
if args.loop:
    m.tool('transport_loop_location_samples',startSample=before['startSample'],endSample=before['endSample']);m.tool('transport_loop_toggle',enabled=True)
m.tool('transport_goto_start');m.tool('transport_play')
pressed=False
try:
    pointer('move',x,False);pointer('press',x,False);pressed=True
    for pos in (x+direction*30,target):
        start=time.monotonic();pointer('move',pos,True)
        deadline=start+3
        while time.monotonic()<deadline:
            observed=metrics()
            if observed['activeGeneration']==observed['requestedGeneration'] and observed['processedFrames']>baseline['processedFrames'] and observed['changedSamples']>baseline['changedSamples']:
                samples.append({'x':pos,'observedHandoverMs':(time.monotonic()-start)*1000,'metrics':observed,'transport':m.tool('transport_get_state')});break
            time.sleep(.01)
        else:raise AssertionError({'metrics':metrics(),'logs':m.tool('ui_get_logs')})
        baseline=observed
    until=time.monotonic()+args.hold_seconds
    while time.monotonic()<until:
        engine_samples.append({'session':m.tool('session_get_info'),'preview':metrics()});time.sleep(.1)
    during=m.tool('track_get_regions',id='488')['regions'][0];assert during==before,(during,before)
    if args.cancel:
        m.tool('ui_press_key',window=state()['title'],key='Escape')
        deadline=time.monotonic()+3
        while any(c['grabbed'] for c in state()['canvases']):
            assert time.monotonic()<deadline,'Escape did not abort drag'
            time.sleep(.02)

finally:
    if pressed:pointer('release',target,True)
    time.sleep(.4);m.tool('transport_stop')
after=m.tool('track_get_regions',id='488')['regions'][0]
if args.cancel:assert after==before,(after,before)
else:assert after['regionId']!=before['regionId'] and after['lengthSamples']<before['lengthSamples'],after
if not args.cancel and args.left_edge:assert after['endSample']==before['endSample'] and after['startSample']>before['startSample'],after
if not args.cancel and args.beat_grid:
    beat_samples=round(initial_session['sampleRate']*60/initial_session['tempoBpm'])
    assert after['lengthSamples']%beat_samples==0,after
assert all(s['transport']['rolling'] for s in samples),samples
final_metrics=metrics()
if not args.cancel and not args.keep_commit:m.tool('session_undo')
restored=m.tool('track_get_regions',id='488')['regions'][0]
if not args.keep_commit:assert restored==before,(restored,before)
m.tool('session_save')
files_added=sorted(str(p.relative_to(session_path)) for p in set(session_path.rglob('*.wav'))-files_before)
report={'previewProjectFilesAdded':files_added,'cancelled':args.cancel,'loop':args.loop,'session':m.tool('session_get_info'),'before':before,'during':during,'after':after,'restored':restored,'samples':samples,'initialSession':initial_session,'engineSamples':engine_samples,'initialMetrics':initial_metrics,'finalMetrics':final_metrics,'logs':m.tool('ui_get_logs')}
path=Path('artifacts/warp-drag-live-validation.json');path.parent.mkdir(exist_ok=True);path.write_text(json.dumps(report,indent=2)+'\n')
if args.cancel:assert not files_added,files_added
else:assert len(files_added)==args.channels,files_added
assert final_metrics['missedWindows']==initial_metrics['missedWindows'],final_metrics
assert all(v['session']['audioEngine']['xruns']==initial_session['audioEngine']['xruns'] for v in engine_samples),engine_samples
assert report['session']['audioEngine']['xruns']==initial_session['audioEngine']['xruns'],report['session']
print(json.dumps({'passed':True,'samples':samples,'metrics':metrics()}))
