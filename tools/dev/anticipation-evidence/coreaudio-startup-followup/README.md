# CoreAudio startup follow-up

## Current result and fast validation

The default native regression command now runs all 13 existing cases concurrently
from verified prepared binaries. With process-specific semaphore names enabled
only in these isolated tests, two consecutive complete passes took **8.24 s and
7.62 s wall-clock**. Assertions, capture lengths and transition events are unchanged.
The command enforces a nine-second execution deadline and reports failure if its
complete measured runtime reaches ten seconds. Build preparation is separate;
stale binaries fail promptly instead of compiling silently.

An initial parallel pass took 8.49 s. Its repeat exposed one constructor failure;
PBD's macOS semaphore implementation opens and unlinks shared logical names.
The test-only interposer scopes those names by PID when explicitly enabled.
Both pre-isolation runs, including the failure, are retained in `fast-regression/`.
Production semaphore code is unchanged, and the hardware probe does not enable
this test namespace. The fast suite uses one realtime graph thread per process,
while future processing remains parallel.

Short CoreAudio capacity runs now default to 512 blocks and completed in **4.16 s
anticipated / 4.55 s direct**, including setup. Their full captures match exactly
(`fast-pair-comparison.json`). At 64 frames, mean callback work is 89.0846 us versus
2,579.25 us. Anticipated rolling xruns/cache misses are zero; three startup xruns
remain. Direct processing records 512 rolling xruns. This is short-run evidence,
not sustained reliability or physical monitoring-loopback qualification.

Earlier corrected ten-minute tests failed at both 64 and 128 frames; details remain
below. The subsequent timing trace was stopped after the user requested tests
under ten seconds. It is explicitly recorded as aborted, not passed. Memory
compression remains an unproven lead. Captures over two seconds now require
`run-native-portable.py ... --soak`; no long run is part of default validation.

The unchanged native anticipative engine starts on the built-in output. Duplex
startup still hangs below Ardour: a small program linked only to Apple's CoreAudio,
AudioToolbox and CoreFoundation reproduces the same failure on BlackHole 2ch and
the Scarlett 2i2. The same program starts, receives callbacks and stops normally
on MacBook Pro Speakers. See `minimal-api/minimal-startup-results.json`.

The minimal program disables input and emits only zero output. It never reads or
stores microphone/system input, changes the default device, or sets the device's
sample rate or hardware buffer. Each failed attempt has a 15-second process alarm.

```sh
clang hal_startup.c -framework AudioToolbox -framework CoreAudio \
  -framework CoreFoundation -o hal-startup
./hal-startup 'BlackHole 2ch'
./hal-startup 'MacBook Pro Speakers'
```

The hang is at `AudioUnitSetProperty(CurrentDevice)` inside CoreAudio's
`AudioDeviceCreateIOProcID` / `_TellServerAboutStreamUsage`, before the application
initializes or starts audio processing. Native and baseline Ardour probes exhibit
the same stack before Session construction. TCC logs show microphone authorization
succeeding and system-audio capture preflight returning undetermined; that is a
correlation, not proof that a missing grant is the cause. The exact server-side
cause needs privileged CoreAudio diagnostics or recovery. `sudo -n sample` could
not authenticate on either test Mac. No permission database was modified.

## Rejected experiments

- Honor input/output None by disabling the corresponding duplex direction. This
  did not clear startup. The source patch was reverted.
- Move AUHAL initialization after device/format/callback setup, following the order
  in [Apple TN2091](https://developer.apple.com/library/archive/technotes/tn2091/).
  This also stalled and was reverted. No CoreAudio backend change remains.
- Give the tiny probe a correctly identified app bundle and usage descriptions.
- Keep an AppKit event loop active while starting audio on another thread.
- Cycle only idle BlackHole's rate from 48 kHz to 44.1 kHz and back. Both property
  changes succeeded; the original rate was restored before the failing open.
- Ask a temporary private aggregate for zero input channels. The API still exposed
  two input channels, so the probe did not open it and immediately destroyed it.

The `experiments/` directory retains source, logs and the rejected backend patch.
The two modified-backend package manifests preceded source-patch provenance in the
packager; their `source_commit` is the base revision, not an assertion that they
contained clean committed backend sources. Subsequent packages record the exact
source diff and its hash. Final native runs use the unchanged backend.

## Native validation

`unmodified-speakers-short/` proves native startup and 1,000 captured blocks with
512 playback effects at 48 kHz / 64 frames. It reports zero cache misses and a
329-us maximum callback, but two total start/playback xruns. This short run alone
does not prove sustained stability.

The capacity probe now supports longer durations and reports both the existing
start-plus-playback xrun count and a separate count after the first captured frame.
No startup error is discarded. Portable runs verify every runtime hash and unchanged
real Ardour preferences. Physical microphone/interface round-trip latency remains
outside this synthetic-input workload.

## Standalone scheduling and observer effects

The first ten-minute built-in-output run (`ten-minute-64/`) completed 450,000
blocks and had no missing cache data, but reported 93 rolling xruns (94 including
startup) and a 10,653-us maximum callback. It is a failed deadline run.
A light direct control also had six rolling xruns in one minute, despite mean/max
callback work of only 61.692/1,222 us. Reducing the existing realtime graph to one
thread did not eliminate the errors. No engine thread policy was changed.

The standalone harness was missing the GUI's `NSActivityLatencyCritical` activity
(`gtk2_ardour/cocoacarbon.mm`, `no_app_nap`). The portable package now injects a
small test-only Foundation dylib making that same request. A one-minute heavy
run then completed with zero rolling xruns, zero cache misses and an 887-us
maximum callback, with two startup xruns still reported. This improvement alone
does not prove causality or sustained stability. The activity did not fix duplex
startup: `activity-blackhole/` still timed out before Session creation.

A second observer effect was found by source inspection: the harness's capture
wait acquired `AudioEngine::process_lock` every two milliseconds merely to read
an atomic progress counter. `AudioEngine::process_callback` uses a try-lock and
emits `Xrun` on contention, so the observer could itself silence callbacks. The
final harness polls that atomic without acquiring the process mutex. Setup checks
of non-atomic Session state retain the mutex. Earlier results are preserved and
not relabeled as passing after this correction.

`playback_xruns` retains its historical meaning of startup plus playback;
`startup_xruns` and `rolling_xruns` partition it at the first captured rolling
frame. `passed` means the capture completed with no queue underruns, not that
hardware delivery was glitch-free. `measured_callback_budget_met` still requires
zero total xruns, zero queue underruns, and an observed maximum within the device
period. Captures are retained locally; committed evidence contains only hashes,
logs and runtime manifests. No physical round-trip latency was measured.

## Corrected short native pair

The activity-only ten-minute run (`activity-heavy-10min/`) still failed with 50
rolling xruns, one startup xrun, zero cache misses and a 7,081-us maximum callback.
The one-minute improvement did not generalize; the activity request alone is not
claimed as a sufficient fix.

After removing the atomic capture poll's process lock, the same packaged runtime
produced this 10,000-block paired result on built-in output at 48 kHz / 64 frames:

| Eight chains × 64 effects | Anticipated | Direct |
| --- | ---: | ---: |
| Mean whole-engine callback (us) | 144.214 | 1,251.71 |
| Observed maximum callback (us) | 299 | 3,042 |
| Rolling xruns | 0 | 7,021 |
| Startup xruns | 1 | 0 |
| Cache underruns | 0 | 0 |

Mean callback work falls by 8.68×. The direct capture contains the computed signal,
not proof of successful device delivery during its xruns. Exact comparison finds
two differing samples, both in the first stereo frame, with maximum error
4.506014688094334e-21; all remaining 1,279,998 samples match. As in the earlier
independent-run comparisons, the delay's injected filter bias affects initial
history. `atomic-pair-comparison.json` still reports `passed: false`; no tolerance
was weakened. The native simultaneous-reference tests remain the exact routing,
recording, MIDI and monitoring correctness witnesses.

## Sustained failure and the profiled scaling bottleneck

The corrected 64-frame ten-minute run (`atomic-heavy-10min/`) still failed:
19 rolling xruns, one startup xrun, 4,464 queue underruns and a 39,764-us maximum
callback. The one-minute 128-frame run passed with zero total xruns, zero cache
underruns and a 299-us maximum. Neither short success substitutes for sustained
validation at the smaller block size.

At 64 frames, redistributing 512 effects into 64 routes of eight effects exhausted
throughput: 2,806,084 queue underruns and 16 rolling xruns in one minute. Selecting
Ardour's existing single realtime DSP-thread setting did not solve this (2,815,840
queue underruns). No realtime thread-policy change was adopted.

A separate explicitly sampled diagnostic run found a concrete shared-data hot
spot. In `profile-top-stacks.txt`, `Plugin::input_streams()` and `output_streams()`
account for 8,423 top-of-stack samples versus 6,801 in the delay DSP. Both getters
call `get_info()`, which returns a shared pointer by value. Instances of the same
plugin share that metadata owner, so these per-block queries unnecessarily contend
on its reference count across render threads. The two-line candidate reads
`_info->n_inputs` / `_info->n_outputs` directly. Each Plugin already owns `_info`;
returned channel counts, plugin lifetime and scheduling are unchanged.

The sampled run is diagnostic only, not a deadline measurement. The full GUI and
library targets build with the candidate; its runtime manifest records the exact
uncommitted two-line production patch. Hardware and native-regression results for
that candidate follow below.

The first one-minute hardware run with that change (`info-read-64-routes/`) reports
zero cache underruns, down from 2,806,084 under the same 64-route / 512-effect
workload. CPU samples fell from roughly 940% to 580%. It still reports two rolling
xruns (eight at startup), and its 2,804-us maximum exceeds the 1,333-us period.
It proves restored producer throughput in this run, not clean hardware delivery.
The candidate is committed as `2e57d0f82c`; the test package records its equivalent
two-line diff against `8c4e2c58f0` because packaging preceded the commit.

The optimized eight-chain / 512-effect ten-minute run (`info-read-heavy-10min/`)
completes 450,000 blocks with zero cache underruns. Mean callback work is 110.664 us,
but three rolling xruns remain (one startup xrun), with a 3,017-us maximum callback.
Producer throughput is restored in this run; this is still not a clean deadline
pass. The native 13-case regression matrix and GUI build pass on the identical
engine library (`source-equivalence.json`, `build-validation.json`).

The optimized paired control uses the same engine on both sides: at 64 frames,
mean callback work is 110.860 us anticipated versus 479.475 us direct (4.325×).
The anticipated short run reports zero rolling xruns versus three direct rolling
xruns. Both captures are finite; their first stereo frame differs by 1.157e-20,
with exact equality for all later samples. The strict comparison remains failed.
The earlier 8.68× pair used the pre-optimization engine on both sides; it is not
the current engine's speedup. The optimized 128-frame one-minute run passes with
zero total xruns and cache misses, and a 270-us maximum callback.

The optimized 64-route / 512-effect one-minute run with the existing
`ARDOUR_PROBE_RT_THREADS=1` setting has zero rolling xruns, zero cache misses and
an 887-us maximum callback. Its nine startup xruns remain in the total. Background
playback workers remain parallel; only the ordinary realtime graph uses one DSP
thread. No global user preference or production thread policy was changed.

## Recorder residency and usable-level signal correction

The optimized single-realtime-thread ten-minute run still has two rolling xruns,
four startup xruns and a 1,735-us maximum despite zero cache misses. Its full
capture scan exposed an independent fixture issue: the legacy 70%-wet / 55%-feedback
preset cascaded through 64 delays builds enormous gain over time (peak about
8.996e11). Those historical captures are stress/profiling evidence, not usable-level
audio. The native simultaneous-reference correctness tests are separate cases.

The capacity fixture now defaults to 5% wet / 10% feedback per delay, with a
unit-sum playback send gain. Every delay remains active; its source executes the
same delay/filter/feedback loop. `ARDOUR_PROBE_LEGACY_GAIN_PRESET=1` reproduces the
old fixture parameters. Hardware output connections are explicitly absent; real
CoreAudio callbacks pace the test, and its synthetic signal is captured internally.
The probe checks the actual device quantum throughout capture, finite samples and
bounded-preset peak below unity. The complete offline scan additionally requires
non-silent one-second windows and finite samples below full scale.

`ARDOUR_PROBE_LOCK_CAPTURE=1` pins only the test recorder's preallocated memory
before timing begins, and releases it at teardown. A 600-second stereo capture
occupies 230.4 MB, so cold pages are a potential observer effect on this loaded Mac.
[Apple's mlock documentation](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/mlock.2.html)
specifies that locked pages remain resident; successful locking is logged and
recorded separately. This removes that potential cause, but does not establish
that historical xruns were caused by page faults. It is not a production engine
memory-locking policy.

The corrected, resident-buffer, bounded-preset short pair at 64 frames uses one
realtime DSP thread in both modes. Anticipated mean/max callback work is
88.4479/184 us with zero rolling xruns, versus direct 2,671.35/4,472 us with
10,000 rolling xruns. Both have one startup xrun. The 5,120,000-byte captures are
bit-identical (SHA-256 `fa2ba4e310d1d3822c25cd9e5f31046c0e72a8eaa1bb4c29f98888c81d1dceee`),
finite, and peak at 0.0762935. This is 30.20× less work on the realtime callback;
playback DSP still executes on parallel background workers, so it is not a claim
of 30× less total computation. The strict comparator passes without tolerance.

The corrected resident-buffer 64-frame ten-minute run still misses the deadline
criterion: two rolling xruns, four startup xruns, and a 2,507-us maximum callback.
It has zero cache misses and the full 28,800,000-frame scan passes: finite samples,
peak 0.0762935, no silent one-second windows. Pinning capture memory did not remove
all deadline misses. No successful sustained 64-frame result is claimed.

The corrected resident-buffer 128-frame ten-minute run also fails: 723 cache
underruns, one rolling xrun (two startup), and a 3,654-us maximum callback against
a 2,666.67-us budget. Its full capture is finite, below unity, and has no silent
one-second windows; these coarse signal checks do not exclude short discontinuities.
The planned paired 128-frame control was not run because this sustained gate failed.
Optional preallocated timing/queue-depth tracing is available in the capacity
probe for short diagnostics; the attempted long traced run was aborted at the
user’s request before it saved a complete trace.

Repeated package manifests are stored once under `runtime-manifests/`; each case’s
`runtime-manifest-ref.json` records the relative path and exact SHA-256.

The concurrent suite also exposed an implicit scheduling assumption in the queue
range oracle: sleeping 150 us did not guarantee a producer publication during
parallel Session construction. That oracle now waits for published slots or a
drain acknowledgement outside `process()`, preserving its 700 iterations and all
assertions. The separate held-producer test still asserts a nonblocking consumer
and off-callback join. A compiled sleep(30) capacity-runner fixture was terminated
and reported failure in 8.55 s (`timeout-harness-result.json` in the follow-up
evidence); this is a watchdog check, not audio evidence.
