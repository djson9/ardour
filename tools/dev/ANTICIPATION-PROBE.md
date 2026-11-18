# Anticipative processing integration and probes

## Default regression command: under ten seconds

After building Ardour, prepare the seven standalone binaries once (or after their
sources/dependencies change):

```sh
python3 tools/dev/validate-native-anticipation.py . --prepare
```

Then run the complete thirteen-case native suite:

```sh
python3 tools/dev/validate-native-anticipation.py .
```

The default command verifies compiled inputs, runs independent cases concurrently,
and enforces a nine-second execution deadline, leaving time to save results before
ten seconds. Stale binaries fail immediately with the preparation command; tests
do not silently rebuild. Preparation caches unchanged probe builds. Compilation
is a separate build step and is not promised to finish in ten seconds.

No test assertions, audio sequence lengths or transition events were removed.
Each isolated process uses one realtime graph thread and parallel future workers.
A test-only, explicitly enabled macOS semaphore namespace prevents concurrent
processes from colliding in PBD's named-sem_open/unlink constructor. Hardware
probes do not enable that interposition. Each case has private preferences, a
separate session, retained logs, and verified binary/header/library fingerprints.
Dummy timing is correctness evidence, not hardware latency qualification.

Capacity probes also default to 512 blocks and a bounded execution timeout.
Captures over two seconds require the explicit `--soak` flag; long hardware runs
are never part of the default regression command.

`jay/anticipation-integration` moves the validated scheduling mechanism into
libardour. `AnticipationQueue` supplies bounded audio/MIDI storage, exclusive DSP
ownership and retained fan-out readers; Session owns `AnticipativeProcess`, which
binds the original registered route processors and their main/send/meter outputs.
The ordinary Graph remains the audible callback graph.

Enable the default-off feature in **Preferences → Performance → Anticipative
Processing**. The render-ahead horizon defaults to 200 ms. The mixer strip context
menu provides **Force Realtime Processing**, the current eligibility reason and
cache underrun count. Global settings persist in Ardour preferences; the track
override persists in session XML. These controls are experimental.

The engine API
`Session::set_anticipative_processing(true, milliseconds)` prepares ordinary routes
on the Butler at playback start. It primes while backend ports are silent, then
consumes cached ranges at the original hardware buffer size. Actual recording and
monitoring controls propagate a live dependency closure. A route can opt out with
`Route::set_anticipative_processing_disabled(true)`; an active queue drains before
its original DSP runs live. No plugin clone or private test DSP chain is required.

Transport events wait asynchronously for producer cancellation before resetting
DSP. The Butler removes bindings and the ordinary stop/locate path restores state.
Processor/port changes, route deletion, presets, export and device changes have
retirement hooks; pending RT reorders/meter changes and loop/playlist edits are
deferred until retirement. A render fault stops and retires through the same path.
Structural edits currently rebuild the whole generation with a brief global silence
and clock hold; uninterrupted recording during these edits is not promised.

The first implementation uses one worker per eligible route. Count-in, external
transport synchronization, reverse/variable-speed transport and offline export use
the ordinary direct path. Surround/interactive/external processing stays live.
Hardware changes and arbitrary third-party plugin state behavior need broader
stress testing; this is an experimental integration, not release certification.

## Native integration evidence

- `anticipation_queue_contract_probe.cc`: fourteen deterministic queue checks,
  including partial MIDI ranges, cancellation, exceptions and slow fan-out readers.
- `anticipation_plan_probe.cc`: eligibility and dependency closure from real controls.
- `anticipation_native_route_probe.cc`: original registered track and bus DSP,
  actual LV2 delays/compressor, main routing or sends, native DiskReader/Butler,
  nonaligned loops, automatic PDC and arm/disarm. `ARDOUR_PROBE_AUTOMATIC=1` exercises
  normal playback-start construction. Combined disk/loop/PDC/send/sidechain output
  matches an independent normal-route reference exactly, with zero cache misses.
- `anticipation_native_transport_probe.cc`: a deliberately held producer cannot
  block callbacks or race a transport reset. Stop and seek restore normal stopped
  processing. Automatic mode additionally verifies restart, live insertion of a
  257-sample latent processor, RT meter relocation, global enable/disable, horizon
  changes, realtime overrides, variable speed and restoration, loop enabling, route
  deletion, and fault stop/restart.
  Backend shutdown precedes Session destruction in the fixture, so its event-pool
  trash collector remains alive while the callback thread exits.
- `anticipation_native_midi_probe.cc`: native MidiTracks and real synth/effects,
  four held-note events across arm/disarm, and exactly-once immediate MIDI delivery
  after ownership transfer. Output matches the normal synth reference exactly.

- `anticipation_native_monitor_probe.cc`: monitor on/off with original AudioTracks;
  after handoff, output matches a normal direct monitor in the same callback while
  upstream playback continues ahead. No extra software buffer at 64/128 frames.
- `anticipation_native_record_probe.cc`: original DiskWriter capture compared with
  an independent raw-input oracle while another real DiskReader track anticipates.
- `validate-native-anticipation.py`: thirteen native cases, including both buffer
  sizes and combined disk/loop/PDC/send/sidechain/gain-automation comparisons.
- `anticipation_native_capacity_probe.cc`: paired normal registered-route capacity
  runs, without the earlier private-chain test bridge; real CoreAudio is selectable.

All probes use an isolated GLib home and check that real Ardour preferences remain
unchanged. Dummy backend evidence establishes correctness, not physical latency or
hardware deadline guarantees. The transport test explicitly waits for backend
warmup: Ardour calls Session during warmup without advancing processed_samples, so
that counter is not a valid callback-progress witness until warmup ends.

`anticipation_engine_graph_probe.cc` retains an unsuccessful earlier adapter: it
cached before the original main output gain/panner and failed exact comparisons.
It is not a passing integration case. Native adapters capture the complete original
output instead. Earlier tests and the capacity evidence below describe the
`jay/anticipation-spike` feasibility branch; its 177 insertions/24 deletions were
host-boundary hooks, not the full engine implementation's size.

## Current validation summary

The complete native 13-case matrix passes on engine `2e57d0f82c`, including 14
queue contracts and six eligibility checks. The earlier 21-case feasibility matrix
also passes against the integrated engine. The full GUI and test targets build,
and the actual Preferences page was rendered and visually inspected.

A 64-route / 512-effect run found that future workers exhausted Ardour's fixed
thread-buffer pool. Queues now own preallocated scratch buffers bound through
`ProcessThread::BufferScope`; ordinary callback threads keep their existing pool.
The same run completes with zero cache misses. Retirement also invalidates prepared
Amp automation pointers before that private storage is freed. The deliberate
exception test now exercises this case with fader automation enabled.

Native capacity probes use the real coordinator and original registered routes.
After silent parameter settling, 512 playback effects plus a live source/shared bus
reduce mean callback work from 859.944 to 113.705 microseconds at 64 frames. Both
runs have zero cache misses. Dummy pacing has deadline outliers and backend xruns;
this is reduced callback work, not hardware stability proof. The exact capture
comparator records a 3.7e-20 difference in the first stereo sample and exact equality
for the remaining 1,279,998 samples. The delay injects a 1e-20 filter bias; a direct/direct repeat differs by 5.2e-20
in that same first stereo sample. Unsettled
startup runs and their larger smoothing difference remain in the evidence.

Native CoreAudio now starts on the local MacBook's built-in output. BlackHole and
Scarlett still stall at device selection; a minimal Apple-only program reproduces
the same IOProc creation hang without linking Ardour. Two attempted backend changes
were ineffective and reverted. No installed DAW or audio service was restarted.
See `anticipation-evidence/coreaudio-startup-followup/README.md` for native hardware
results, failed controls and startup isolation. The older CoreAudio numbers under
"CoreAudio capacity breakthrough" below belong to the earlier feasibility path.

The standalone native capacity harness now requests the same macOS latency-critical
activity as the GUI. It also polls its atomic capture counter without taking the
engine process mutex: the earlier polling lock could make the callback try-lock
fail and manufacture xruns. Non-atomic setup checks remain protected. Duration-based
timeouts support ten-minute runs. `ARDOUR_PROBE_RT_THREADS` selects the existing
processor-usage setting for controls; no production thread policy changed.
`ARDOUR_PROBE_ALLOW_APP_NAP=1` omits the activity request for a diagnostic control.

`playback_xruns` is the total since the measured startup began. `startup_xruns` and
`rolling_xruns` partition that count at the first captured rolling frame. No errors
are removed from the total. A capture `passed` result is not a hardware deadline
pass; inspect the xrun, queue-underrun and observed callback-maximum fields.

Evidence: `anticipation-evidence/native-integration/validation-2026-11-17/README.md`.
After preparation, reproduce native correctness with:

```sh
python3 tools/dev/validate-native-anticipation.py .
# Separate build/package preparation for optional hardware capacity checks:
python3 tools/dev/run-anticipation-probe.py . anticipation_native_capacity_probe.cc --build-only
python3 tools/dev/package-native-capacity.py
```

Save each capacity result before running the other mode, then compare their capture
files with `compare-native-capacity.py AHEAD_RESULT DIRECT_RESULT`. It requires exact
finite equality and exits nonzero for any difference. `package-native-capacity.py`
and `run-native-portable.py` support isolated arm64 macOS reproduction without
installing the build; manifests verify every relocated binary and the real user
configuration checksum is checked. The probe asserts its private GLib home.

For built-in-output native capacity reproduction (synthetic live input, not a
physical monitoring loopback), after packaging:

```sh
ARDOUR_PROBE_CONSOLE=1 ARDOUR_PROBE_OUTPUT_DEVICE='MacBook Pro Speakers' \
  ARDOUR_PROBE_RT_THREADS=1 ARDOUR_PROBE_LOCK_CAPTURE=1 \
  python3 tools/dev/run-native-portable.py \
  artifacts/native-capacity-portable ahead
```

This captures 512 blocks at 48 kHz / 64 frames, with one realtime graph thread
and parallel playback workers. Historical ten-minute cases can be reproduced only
by explicitly setting `ARDOUR_PROBE_BLOCKS=450000` and appending `--soak`; these
are separate diagnostics, not routine tests. The equivalent GUI setting is **Performance → DSP
CPU Utilization → Signal processing uses → 1 processor** (restart required).
Capture memory is pinned only inside this test; locking failure fails the run.
The capacity fixture now uses 5% wet / 10% feedback per active delay and unit-sum
playback sends, so large cascades stay at usable levels. The legacy 70%-wet /
55%-feedback fixture built excessive gain in long captures; reproduce it only
with `ARDOUR_PROBE_LEGACY_GAIN_PRESET=1`. Synthetic stress audio is captured
internally, with no physical output connections. The hardware buffer size is
checked throughout the run. `scan-native-capacity.py RESULT_JSON` (NumPy, test-only)
checks the complete capture for finite samples, full-scale bounds and non-silent
one-second windows after the timed run ends.

Run `direct` separately for a paired
control; never overlap capacity runs. `compare-native-capacity.py` also accepts the
portable runner's nested results and requires the same runtime manifest, workload,
length and exact finite samples. Package manifests include the production source
patch hash and harness source hashes, as well as every relocated binary hash.

## Native hardware scaling follow-up

The first sustained native hardware runs exposed two probe problems (missing GUI
activity setup and an unnecessary process-lock poll) and then a real throughput
bottleneck. At 64 routes / 512 effects, `Plugin::input_streams()` and
`output_streams()` repeatedly copied shared plugin metadata ownership. Profiling
found more top-of-stack samples in those two getters than in the delay DSP.

Commit `2e57d0f82c` replaces those two temporary shared-pointer copies with reads
through the Plugin's existing `_info` owner. The channel counts and ownership
contract are unchanged. The same one-minute 64-route hardware run then has zero
queue underruns, versus 2.8 million before, though two rolling xruns remain. No
thread-pool, worker-priority, queue-wakeup or CoreAudio backend change was needed.
The full native 13-case matrix and GUI build pass with this optimization.
With the optimized engine on both sides of the 64-frame paired comparison, mean
callback work falls from 479.475 to 110.860 us (4.325×). Its 128-frame one-minute
run has zero total xruns and cache underruns. The optimized default-thread-count
ten-minute run still has three rolling xruns despite zero cache misses; do not
call that a deadline pass. Selecting the existing one-realtime-DSP-thread setting
produces a one-minute 64-route run with zero rolling xruns and cache misses, an
887-us maximum callback, and nine separately reported startup xruns. Background
playback remains parallel. Full results, failed controls and exact package/source
hashes are retained in
`anticipation-evidence/coreaudio-startup-followup/`.

The corrected resident-buffer, bounded-preset short pair at 64 frames / one realtime
DSP thread now passes the exact comparator: both 5,120,000-byte captures have
SHA-256 `fa2ba4e310d1d3822c25cd9e5f31046c0e72a8eaa1bb4c29f98888c81d1dceee`,
with finite samples and a 0.0762935 peak. Mean callback work is 88.4479 us anticipated
versus 2,671.35 us direct (30.20× less callback work, not less total DSP). Playback
xruns are zero anticipated versus 10,000 direct; each has one startup xrun.
The historical gain-stress measurements above are preserved but are not usable-level
audio qualification. Consult the latest evidence for sustained results.

## CoreAudio capacity breakthrough

At 48 kHz / 64 frames, eight chains of 64 real LV2 effect inserts feed a shared
bus with four effects and a live source. Two paired runs produced identical
1,048,576-byte sample captures (SHA-256
`125056af1ac2b0bf1510f942b2bba57428acc367e8d25dc9d6e093fa34f3873d`).
Anticipated processing had zero cache misses and zero backend xruns in both runs,
with whole-engine mean 79.68 / 79.48 µs and observed maxima 373 / 334 µs. Direct
processing had 51 / 99 backend xruns, mean 1,071.97 / 1,089.80 µs and observed
maxima 4,463 / 4,343 µs. Each run covers 2,048 blocks (2.731 seconds of music).

Plugin resets for an identical starting state happen off-callback with the Session
detached. Calling PluginInsert::flush instead merely queues a large delay-buffer
clear for the next callback; that caused the earlier startup spike. Removing all
resets avoided the spike but gave unequal initial states, so those exploratory
runs are preserved separately. The final pair fixes both issues.

Evidence and runtime/source manifests are under
`tools/dev/anticipation-evidence/coreaudio-evidence/`. The final four capture hashes
agree; prior failed and exploratory runs remain in the summary. A deliberate
768-insert overload produced 3,074 cache misses and is explicitly a failed workload.
Fast callback timing alone is never sufficient evidence of successful playback.

These are real CoreAudio callbacks using BlackHole, with a synthesized live input;
they do not measure physical microphone/interface round-trip latency. They prove
the capacity mechanism in short isolated tests, not sustained release stability.

## Expanded graph and latency evidence

The twenty-one-case validation matrix passes on engine revision `5f4b5bd4e3`. It includes
the original disk/monitoring/MIDI/send cases, cached buses with an actual active
compressor sidechain, private main output at 64/128 frames, automatic latency
compensation at both sizes, six eligibility decisions using real controls,
ongoing disk refill, seek/edit generations, actual recorded files, nonaligned loops
with scoped-clock witnesses and MIDI crossing the cache boundary at both sizes.
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
smoothing. A timestamp-dependent gain on the latent processor then exposed the
opposite placement of own-processor latency in `bounce_process` and normal
playback: before-fix output error was 0.00185462. `Route::render_process` now reuses
the same processing loop with normal playback timestamps, retaining the existing
bounce timing for export. Both 64/128-frame time-sensitive tests pass exactly.
ProcessThread::RenderScope supplies project/continuous sample mapping to the
VST3 adapter. The loop witness validates the mapping and restoration across
ownership changes, but native vendor VST3 DSP compatibility remains untested.

`anticipation_eligibility_probe.cc` reuses `Route::feeds` and existing graph edges
for record-arm, explicit input monitoring, forced-live buses, sidechain propagation
and disconnection. It validates the classification, not a shipped coordinator.

`ARDOUR_PROBE_REFILL=1` sets a custom half-second playback buffer after Session
construction, checks the Butler's actual 24,000-sample buffer setting, and refills
real private AudioTrack readers on a background thread using existing refill
methods and their thread-local scratch allocation. Tests require repeated buffer
increases and zero DiskReader underruns as well as exact input/output. The 128
frame standalone run observed 60 refills. Production can wake the existing Butler;
the helper thread only supplies otherwise unregistered fixture tracks.

`anticipation_seek_probe.cc` discards outstanding future blocks, completes actual
Session locates to 0, 240000 and 48000, and inserts another actual plugin before
the third generation. It stops/join workers only off the callback after suspending
consumption, reuses the same DSP instances, and verifies exact output in all
generations. It uses a stopped-transport barrier and does not measure rolling-seek
gaps or seamless looping. A stronger startup barrier checks deferred events and
Butler work as well as locate completion: Session buffering setup can queue Stop
events that otherwise execute after the test begins rolling.

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

## Additional recording, clock and MIDI boundary cases

`anticipation_record_probe.cc` leaves actual DiskWriter capture in the registered
track's callback while DSP ownership changes. Recorded files match raw input and
the final mix exactly at 64/128. Record arming precedes playback in this fixture;
the separate control fixture exercises dynamic record-enable requests.

`anticipation_loop_probe.cc` uses actual Session loop boundaries, including their
beat/sample rounding and split callbacks. Its ClockWitness checks continuous time
across wraps and ownership changes and verifies restoration before downstream DSP.
`anticipation_midi_boundary_probe.cc` caches MIDI events for a normal callback synth,
requiring four exact event offsets, identical output and a quiet release tail.

Real CoreAudio/BlackHole callback runs now pass the actual-arm/disk/send fixture at
64 and 128 frames with zero xruns, exact audio and no cache misses. Portable runtimes
contain only task-owned copies with source/package hashes; run-portable-probe.py
verifies packaged runtime hashes and unchanged user preferences. These are short
virtual-device runs, not physical round-trip latency or sustained stability tests.

## Remaining production gates

The Session coordinator, normal controls and native lifecycle integration described
in the original feasibility gates are implemented. Remaining release validation:

1. Sustained native CoreAudio/interface runs and physical monitoring loopback.
2. Arbitrary vendor-plugin compatibility, preset/export/device-change stress, and
   repeated session teardown under load.
3. Larger route-count CPU scheduling measurements; one producer per eligible route
   remains an intentional first-version limit.
4. Evaluate the whole-generation structural-edit pause before promising seamless
   edits during recording. Monitoring transitions use the measured drain behavior.

The concurrent suite also exposed an implicit scheduling assumption in the queue
range oracle: sleeping 150 us did not guarantee a producer publication during
parallel Session construction. That oracle now waits for published slots or a
drain acknowledgement outside `process()`, preserving its 700 iterations and all
assertions. The separate held-producer test still asserts a nonblocking consumer
and off-callback join. A compiled sleep(30) capacity-runner fixture was terminated
and reported failure in 8.55 s (`timeout-harness-result.json` in the follow-up
evidence); this is a watchdog check, not audio evidence.
