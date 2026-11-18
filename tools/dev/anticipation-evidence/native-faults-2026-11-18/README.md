# Short native fault coverage — 2026-11-18

The final **25-case suite passes in 8.99 s wall-clock** (8.903825 s inside the
validator), using the Realtime Dummy backend on the build Mac. Two earlier full
passes took 8.95/8.83 s wall-clock before an additional finite-sample assertion in
the stall oracle. The final run includes that assertion. Compilation is a separate
cached preparation step. No production engine code changed in this update.

The suite establishes the exercised signal, timing, ownership and recovery
contracts. It does **not** establish sustained hardware reliability, physical
round-trip latency, or arbitrary third-party plugin compatibility. The corrected
ten-minute hardware gate remains failed; no long soak was restarted.

## Reproduce and iterate

From a configured native Ardour build checkout:

```sh
python3 tools/dev/validate-native-anticipation.py . --prepare
python3 tools/dev/validate-native-anticipation.py .
python3 tools/dev/validate-native-anticipation.py . --list
python3 tools/dev/validate-native-anticipation.py . --case 'stall-demotion-*'
python3 tools/dev/validate-native-anticipation.py . --case 'oracle-reject-*'
python3 tools/dev/validate-native-anticipation.py . --case 'midi-*' --original-sequences
```

`--case` accepts repeatable shell-style patterns and also selects preparation.
Stale preparation fails promptly. Execution has a nine-second deadline and fails
if total measured time reaches ten seconds; this is a bound on the test run, not a
guarantee that an overloaded machine will pass. At most eight engines run at once,
with longer sequences first. Preferences and semaphore names are isolated per case.
Failure output includes diagnostic tails, per-case durations, and retained files.

The default route/MIDI/recording profile uses 1,024 blocks instead of 2,048 and
rescales arm/disarm and MIDI events. All existing assertions and semantic events
remain. MIDI checks ensure a full 200 ms horizon plus margin fits between relevant
events. Original-length standalone probes remain the default without the compact
environment flag. The targeted original-length MIDI pair passes in 7.53 s wall-clock.
Monitoring and lifecycle sequence lengths were not shortened.

For investigation, start with one scenario/quantum, inspect its first differing
frame against the queue trace, then test the paired quantum. A missing result or
expired watchdog is a failure. Once a hypothesis is resolved, run the full matrix;
do not use repeated passing tests as a substitute for diagnosing a failing case.
The rejected approaches below demonstrate that loop in this change.

## What the new fixture measures

A registered playback track runs four real LV2 delay inserts through Ardour's
original Session/Graph/Route path, alongside an uninterrupted reference. A separate
MonitorInput track receives an analytic current-cycle signal. A pre-plugin marker
holds only the background worker until a specified audible sample position; it
never intentionally sleeps the callback. A two-second watchdog catches deadlock.
A stopped 8,192-sample warmup settles native gain smoothing before measurement.

Each scenario captures 48,000 rolling samples. Its preallocated CSV records queued
frames/coverage, underruns, generation, queue state, worker hold state, audio errors,
callback gaps and render duration. The oracle resolves the current queue each
callback because startup/transition plan retirement can change its generation.
All samples must be finite. Outside diagnosed missing blocks playback is exact;
missing blocks must be silence and agree with underrun counters. The independent
monitor path must remain exact throughout. Recovery is bounded to within 100 ms of
audio after release, and lag beyond a whole cache exercises stale-block disposal.

Final measured outcomes (48 kHz):

| Scenario | 64-frame loss | 128-frame loss | Additional observation |
| --- | ---: | ---: | --- |
| Absorbed stall | 0 ms | 0 ms | Cache covers the held worker; no misses. |
| Starved | 32 ms | 32 ms | First hole 24,576; recovery 26,112 at both quanta. |
| Long-starved | 252 ms | 253.333 ms | Loss exceeds full 200 ms cache; recovery 36,672 / 36,736. |
| Record-arm while held | 0 ms | 0 ms | One exclusive handoff; first live frame 24,640 / 24,704. |
| Live-to-ahead demotion | 30.6667 ms | 29.3333 ms | First job has no reserve; exact recovery at 22,016. |

Every positive case has zero ownership races and exact monitored-input output.
The ordinary starvation pair loses 24 blocks at 64 frames versus 12 at 128:
compare **lost audio duration**, not raw miss counts.

Demotion is a known limitation, not a dropout-free success criterion. The test
requires a diagnosed gap when the first future job is deliberately delayed. It
proves bounded recovery and monitoring isolation, not that the gap is acceptable
for release. No production mitigation was implemented here.

Two negative controls require the oracle to reject deliberately wrong fixture
output: a one-block monitor delay at frame 20,480 produces a 0.366821 maximum error;
a stale recovered playback block produces a 0.00090607 maximum error. The unaffected
lane remains exact, and each failure must begin at the injected frame. Their test
status is success **because corruption was detected**.

## Evidence and pivots

Each directory retains its summary and condensed compiled-input provenance.
`final/` also includes every case log and all twelve fault/negative-control traces.
`original-midi/` retains both logs. Historical failures retain available failed-case
logs. Raw captures, binaries, private homes and repeated full dependency manifests
are intentionally excluded from source control.

| Directory | Validator seconds | Result and response |
| --- | ---: | --- |
| `initial-monitor-startup-failure` | 2.688 | Six failures: native startup gain smoothing disagreed with the analytic input oracle. Added stopped warmup; no measured samples skipped. |
| `fixed-monitor-warmup` | 2.842 | Six controlled cases pass. |
| `unbounded-deadline-failure` | 9.070 | Starting all 21 engines together exceeds deadline. |
| `double-speed-deadline-failure` | 9.062 | Double Speed fails to improve combined runtime; rejected. |
| `compact-unbounded-cache-misses` | 6.671 | Shorter sequences finish, but routing/MIDI at 64 each report a cache miss; not accepted as a passing run. |
| `isolated-routing-control` | 2.426 | Routing-64 alone passes; supports contention as a lead, not a proof of all miss causes. |
| `bounded-21-pass` | 7.733 | Cap engines at eight, longer-first scheduling; all 21 pass. |
| `demotion-oracle-failure` | 2.724 | Added long-starvation and demotion pair; retained raw queue pointer was invalid across startup plan retirement. |
| `fixed-demotion-oracle` | 2.722 | Resolve queue per callback and trace generations; all four new cases pass. |
| `repeat-1` | 8.865 | All 25 pass. |
| `repeat-2` | 8.704 | All 25 pass again. |
| `original-midi` | 7.452 | Original 2,048-block MIDI pair passes. |
| `final` | 8.904 | All 25 pass with explicit finite-sample guard. |

The unbounded failures motivated concurrency control instead of weakening the
zero-miss assertions in normal cases. Host scheduling still affects elapsed time
and catch-up latency. Controlled faults localize the boundary failure much faster
than waiting for a spontaneous hardware dropout, but do not explain the earlier
ten-minute hardware failures. The existing memory-pressure lead remains unproven.

## Provenance

Build checkout: `/Users/davidson/workspace/ardour-anticipation-spike`, branch
`jay/anticipation-integration`. Runs were made against base commit
`5c32c1715b0c831468ac90b76e152f5ca183bbde` with the test changes included in this
commit uncommitted. Exact compiled test inputs and probe/helper hashes are in
`compiled-provenance-summary.json`; summaries retain the validator hash. The
isolated-routing historical run overlapped a validator edit, so its reported
validator hash is not proof of the exact runner text used for its entire execution.

Final validator SHA-256:
`323707f8c047967191ba5e078d5b2157737246ad0616acd28c073c586e68128e`.

Unchanged libardour SHA-256:
`a843fc5d0d2db78f746ed1b8796aceca3702b79e0f9e72b1cb8c5c1272c408b6`.
The GUI/library build evidence from engine commit `2e57d0f82c` remains applicable
because this change modifies only probes, their runner, and documentation/evidence.
Final execution verifies unchanged library and real user preferences. Test traces
are synthetic input, not user recordings.
