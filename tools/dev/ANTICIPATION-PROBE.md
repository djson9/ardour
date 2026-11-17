# Anticipation feasibility probes

These standalone macOS probes link the existing Ardour build. They reuse real
Routes, PluginInsert/LV2 effects, sends, returns and (in the callback probe) the
normal Session processing path. They are experiments, not an enabled DAW feature.
The branch has narrow hooks for cached InternalReturn and SideChain input,
independent InternalSend cycle ownership, private MainDelivery output buffers,
and automation evaluation before the audible transport rolls. The coordinator
and fixtures still live in standalone experiments; this is not a finished feature.

The current engine diff is 117 insertions and 22 deletions across 12 files. That
is the host-boundary support, not the complete feature's code size: queueing,
ownership, eligibility and transport coordination also need production code.

## Expanded graph and latency evidence

The eleven-case validation matrix passes after the Delivery changes. It includes
the original disk/monitoring/MIDI/send cases, cached buses with an actual active
compressor sidechain, private main output at 64/128 frames, automatic latency
compensation at both sizes, and six eligibility decisions using real controls.
All audio comparisons have zero final-output error and zero queue underruns.
Most dummy runs exceed their nominal callback period; correctness is reported
separately from deadline success. Physical I/O latency remains unmeasured.

`anticipation_graph_probe.cc` exercises cached A feeding cached B while a live
source feeds the common output. The compressor receives the cached key at its
normal SideChain boundary and produces roughly 20 dB peak gain reduction.
`ARDOUR_PROBE_MAIN_OUTPUT=1` includes the original gain/panner/Delivery processing
with owned output storage; an independently connected backend-port observer sees
zero future-audio leakage. Dependency handoffs preserve the existing DSP state.

`anticipation_pdc_probe.cc` compares the cached slice against a normal registered
Route containing the same real LV2 chain and an actual 257-sample delay. Ardour's
existing compensation supplies 320/384 samples of startup preroll and initial
63+1 / 127+1 subcycles. The cache uses those timeline chunks and reports 257
samples, never the 9,600-sample anticipation horizon. Both output comparisons
are exact. Independent oracle instances are flushed on their first moving call
because stopped callbacks otherwise advance only the normal route's plugin
smoothing. The synthetic delay does not exercise a third-party latent plugin's
own host-time interpretation; `bounce_process` and normal processing currently
apply a processor's own latency on opposite sides of its call.

`anticipation_eligibility_probe.cc` reuses `Route::feeds` and existing graph edges
for record-arm, explicit input monitoring, forced-live buses, sidechain propagation
and disconnection. It validates the classification, not a shipped coordinator.

## Run

```sh
python3 tools/dev/run-anticipation-probe.py .
python3 tools/dev/run-anticipation-probe.py . anticipation_callback_probe.cc
python3 tools/dev/validate-anticipation.py .
```

The runner verifies compatible library sources and records library/source hashes,
working-tree status, logs and JSON under `artifacts/anticipation-probe/` or
`artifacts/anticipation-callback-probe/`. It deletes a stale result before running.
A process-local GLib home interposer isolates Mac preferences without changing
HOME; the executable checks isolation before initializing Ardour, and the runner
checks the real user-config checksum afterward. It never operates the user's GUI.

Use a build of this branch: the InternalReturn layout differs from the original
main-checkout build. After configuring with the normal optimized macOS options,
the targeted build is:

```sh
python3 waf build -j4 --targets=libardour-tests,dummy_audiobackend,a-delay,reasonablesynth,libardour_pan1in2out,libardour_pan2in2out,libardour_panbalance,libardour_panvbap
python3 waf build --targets=ardev_common_waf.sh,../../LV2/a-delay.lv2/manifest.ttl,../../LV2/a-delay.lv2/a-delay.ttl,../../LV2/reasonablesynth.lv2/manifest.ttl,../../LV2/reasonablesynth.lv2/reasonablesynth.ttl
```

Waf's source-distribution detection currently mistakes a Git worktree's `.git`
file for the absence of Git. Generate its ignored `libs/ardour/revision.cc` with
the actual worktree revision if configuration reports a missing revision file.

## REAPER behavior: drain before live ownership

The companion `reaper-handoff` scripts launch a separate REAPER instance with
isolated preferences and a deterministic JSFX trace. They compare an anticipative
track with an always-live reference, arm at two seconds and disarm at four seconds.
Only the exact process launched by the harness is terminated afterward.

```sh
python3 tools/dev/reaper-handoff/run.py --output artifacts/reaper-handoff --horizon 200 --block 64
python3 tools/dev/reaper-handoff/analyze.py artifacts/reaper-handoff
```

Installed REAPER 7.81/macOS-arm64 measurements:

| Horizon / device block | Ahead lead | Processing pause on arm | Reported monitoring activation | Lead while monitored |
| --- | --- | --- | --- | --- |
| 50 ms / 64 | 49.33 ms | 50.40 ms | 60.07 ms | 0 ms |
| 200 ms / 64 | 198.67 ms | 201.18 ms | 210.02 ms | 0 ms |
| 500 ms / 128 | 498.67 ms | 501.25 ms | 510.06 ms | 0 ms |

Plugin timeline positions and accumulated sample counters remain contiguous.
Disarming restores the configured lead. Monitoring-state polling has roughly
30 ms UI granularity; DSP timestamps measure the pause independently. This
supports draining queued playback to the existing DSP cursor before changing
ownership. It does not establish every REAPER routing mode or physical I/O latency.

The earlier assumption that matching REAPER required an instantaneous rewind of
arbitrary plugin state was unnecessary for this fixture. Monitoring activation
delay and steady-state input/output latency are different measurements.

## Correctness probe

At 64 and 128 frames:

- Stateful test processor through a bounded queue: exact direct/cached equality.
- Naive future-to-earlier-state handoff: maximum difference 0.316799 (expected failure).
- Reset/replay of the known test state: exact equality, but not a proposed solution.
- Ordinary InternalSend retains one current block: historical return error 0.0557618.
- Four actual `urn:ardour:a-delay` inserts per source: exact cached/direct equality
  across drain-to-cursor ownership transfer; effect-versus-dry difference 0.293145.
- Replay post-FX sends at consumption time, mix with a live source, and process
  through InternalReturn plus four actual delay inserts on the destination bus:
  exact equality with the direct reference, nonzero output peak 0.336858.

This harness can wait and join. It proves signal/state behavior, not deadlines.
Its post-FX-send result does not yet cover arbitrary mid-chain taps, sidechains,
compensated routing, MIDI, or anticipative destination buses.

## Normal callback probe

The worker owns a private Route using the same existing processing slice. A small
bridge Processor on a registered route consumes its bounded queue without waiting.
The ordinary Session/Route callback runs live source and bus processors, delivery,
InternalSend and InternalReturn. A simultaneous direct reference follows the same
normal routing path, including normal transport-start gain ramps.

At callback 512 the bridge requests producer acknowledgement. It continues
consuming cached blocks, then transfers the same DSP instance at the final
published cursor. At block 1024 it releases ownership to the worker after finishing
the live block; the worker rebuilds lead while consumption continues. No callback
joins, waits for queue data, or replays old plugin history.

The first completed 64-frame run verified 2,048 bus blocks with zero sample error,
zero queue underruns and zero measured bridge-duration overruns. The largest
bridge call in that run was 361.542 microseconds against a 1,333.33 microsecond
block period. This measures only the bridge, not total callback time or physical
latency. The initial 128-frame normal-priority dummy run retained exact equality
but did not finish within its nominal-duration timeout; a subsequent realtime-dummy 128-frame run completed all 2,048 blocks with zero error and underruns, and a 69.042 microsecond maximum bridge call. It took 6.54 wall-clock seconds for 5.46 seconds of timeline, so dummy pacing still does not prove hardware deadlines.

The controller is scripted at fixed callback indices. Automatic record-arm
eligibility, full route migration, plan invalidation and GUI editing are still
unimplemented. Four delay inserts are a stateful compatibility fixture, not proof
of massive-chain capacity.

## Capacity experiment

```sh
ARDOUR_PROBE_DIRECT=1 python3 tools/dev/run-anticipation-probe.py . anticipation_capacity_probe.cc
python3 tools/dev/run-anticipation-probe.py . anticipation_capacity_probe.cc
```

The fixture uses eight independent chains of 32 actual LV2 delay inserts, a live
source and a shared bus with four more delay inserts. The existing parallel
processing graph is present (13 DSP threads); this is not a comparison against a
single-thread-only engine. Source generation is deterministic test DSP. Captures
are taken at the bus, before physical delivery. The stress patch produces peaks
above unity, so it is not an auditionable mix or a hardware-output test.

| 64-frame result | Direct | Anticipated |
| --- | --- | --- |
| Engine mean callback | 487.960 us | 80.6996 us |
| Engine maximum callback | 19,028 us | 343 us |
| Per-source bridge calls over 1,333.33 us | 8 | 0 |
| Cache underruns | N/A | 0 |
| Compared stereo samples | 262,144 | 262,144 |

Both captures have SHA-256
`70be909e85f2c16d43d6e1c639fb088f5ffa9418eb403825dccae459a1989543`:
the output is bit-for-bit identical. `capacity-evidence.json` records the builds,
source hashes available at the time, metrics and limitations. These short runs
prove the worker queues can remove substantial work from the callback while
preserving the tested signal path. They do not establish universal plugin
capacity, physical latency, long-run hardware stability or full REAPER parity.

The callback timing audit also found that AudioEngine resets performance meters
when its initial warmup ends. Startup peaks and final steady-state maxima are
therefore distinct; the callback probe now reports where it observed the peak.
A later small-fixture run saw its pre-reset peak at block 42 and a post-warmup
engine maximum of 616 us. Do not describe every startup or OS scheduling spike as
an anticipation failure, or omit measured steady-state overruns.

## Failures that changed the experiment

- Session construction automatically attaches to AudioEngine. The original
  correctness harness incorrectly assumed otherwise; longer mixed-bus runs
  exposed concurrent dummy processing. It now explicitly detaches before creating
  fixtures and asserts the detached state. The corrected rerun passes the above
  signal comparisons. Earlier unattached-session claims are superseded.
- A real LV2 worker crashed before its thread-local tempo map was initialized.
  Calling the existing `Temporal::TempoMap::fetch()` on worker entry fixed it.
- Comparing normal callback output with a bounce-only bus oracle includes different
  transport-start processing. The callback test now uses a simultaneous normal
  callback reference instead of ignoring startup errors or weakening tolerance.
- A CoreAudio/BlackHole attempt stalled inside the macOS audio service during
  backend startup, before the Session or callback experiment. Only its probe
  process was terminated; this is not a successful CoreAudio test.
- An early unisolated startup detected a changed real Ardour config checksum. Its
  original contents were not captured and were not blindly restored. Subsequent
  successful isolated runs verify the original config is unchanged during them.

## Expanded input and routing tests

`validate-anticipation.py` records each run's complete log and hashed JSON in a
new timestamped directory. Its matrix covers:

- Actual AudioTrack/DiskReader playback, actual queued record-enable controls,
  current-cycle monitored input, plugin automation, and a cached mid-chain send,
  with two repeated 64-frame runs and a 128-frame run.
- Reasonable Synth plus four real delays, with notes held across both ownership
  transitions and note-offs at nonzero block offsets. It checks nonzero synth
  output and a decayed release tail, not just event delivery.
- A 187-sample InternalSend compensation delay with file/live switching,
  automation and a cached mid-chain send at 128 frames. This exercises the delay
  line, not yet automatic graph-wide PDC.

The actual-track test preloads 12 seconds of media buffering for its short run.
It does not yet exercise continuous butler refill. Record-enable events are
requested from the control thread through Ardour's real control API; the bridge
observes that control, drains, and applies effective monitoring at its handoff
cursor. This proves a control-driven candidate inside the fixture, not general
recording, downstream eligibility or arbitrary route migration.

The startup barrier matters: the Session must process its queued initial controls
and locate work before the worker pre-renders. A stale global locate flag otherwise
made DiskReader cache silence for the entire initial horizon. Waiting while the
Session remains detached cannot refresh that flag. Diagnostics must compare the
beginning of the stream as well as steady playback.

Two other failures established the need for the narrow engine hooks. Mixing a
cached tap through an added generic Processor bypassed Ardour's mandatory trim
stage; mixing through InternalReturn restores exact ordering. Global hardware
CycleStart notifications also reach private InternalSends, so background-owned
sends now prepare their buffers explicitly. Finally, moving plugin automation
must follow the rendered range even before the audible session starts rolling.

`passed`/`correctness_passed` are signal and transition checks. The separate
`measured_callback_budget_met` field includes observed whole-engine peaks. Several
expanded runs miss that budget despite correct output and no missing cached
blocks. None establishes physical monitoring latency or hardware stability.

## Remaining gates

1. Establish correctly paced 64/128-frame execution and total callback deadlines;
   then show an actual workload that fails direct and succeeds anticipated.
2. Cover cached buses, sidechains and automatic latency propagation, retaining
   the existing routing code and testing cache timestamps under compensation.
3. Cover ongoing disk refill, MIDI routing/input, seek/loop and generation
   cancellation. The isolated REAPER seek fixture resets JSFX state at a seek;
   this differs from its state-continuous monitoring handoff.
4. Integrate general route eligibility and downstream promotion, replacing the
   fixture's controller with safe production mutation/deletion ownership.
