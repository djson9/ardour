# REAPER comparison and deferred demotion — 2026-11-18

This update fixes **unclaimed** live-to-ahead demotion. Previously the last live
callback immediately set `Ahead`; if the worker was not scheduled, the next
callback returned silence from an empty cache. Now it offers the current DSP
cursor to the worker. The worker and next callback atomically compete for that
offer. Until the worker claims it, the callback keeps rendering live at the normal
hardware quantum. A later worker claim resumes anticipation from the exact cursor.

The queue change is small: a new `Offered` state with atomic producer/consumer
claims. The coordinator also preserves the live dependency closure until the
source leaves live/offered/draining ownership. This lets dependent buses continue
using current input while the source worker is unavailable. There is no added
wait, clone, state serialization, allocation, or future DSP render on the callback.
Both whole-block and partial-block drain paths publish the offer.

This does **not** promise to conceal a plugin that stalls after the worker claims
ownership. The same stateful DSP cannot safely execute concurrently. Those cases
retain diagnosed playback silence, bounded recovery, and independent live-monitor
checks. The previous `stall-demotion-*` cases deliberately hold an already-entered
plugin, so they remain overload/recovery tests rather than zero-gap tests.

## What REAPER does in the observed fixture

We re-read the existing REA/Ghidra inspection of installed REAPER 7.81 ARM64.
Its cache-aware renderer checks cached output, coordinates ownership, and has
both a no-cache rendering path and waits while another renderer is active. The
saved renderer evidence ID is
`ev_c8937f92800e268f8588c245890042f38399e1e3a301b998081bc44cc6527840`;
the inspected slice SHA-256 is
`5efb42a1b8d7cfae0bf5fb09e94d8246e3ba352cf35723971660438d2790f9a2`.
This is behavioral guidance, not copied proprietary implementation.

Four new isolated runtime captures arm/disarm a track while an always-live
reference processes alongside it. Normal disarm restores approximately the
configured 200 ms rendering lead with contiguous plugin timeline/sample counters.
A JSFX one-shot stall injected at disarm interrupts processing on the reference as
well as the target:

| Buffer | Injection | Measured plugin hold | Maximum nearby always-live processing gap | Restored lead |
| --- | --- | ---: | ---: | ---: |
| 64 | None | 0 ms | 1.354 ms | 200 ms |
| 64 | Requested 32 ms | 28.492 ms | 28.854 ms | 200 ms |
| 128 | None | 0 ms | 2.727 ms | 197.333 ms |
| 128 | Requested 20 ms | 20.000 ms | 22.842 ms | 197.333 ms |

The 64-frame JSFX loop returned before its requested duration; report the measured
28.492 ms, not 32 ms. All captures retain contiguous plugin sample/timeline
counters. These are plugin processing timestamps, **not physical audio captures,
xrun counts or physical round-trip latency**. The injected call begins one block
ahead of the last reference position. We did not instrument thread identity, so
this does not prove which private wait branch or renderer thread handled that call.
It does show that the tested transition does not isolate the reference from an
already-running plugin stall. The static wait paths are consistent with that
observation; their exact reachability remains an inference.

The public [REAPER API](https://www.reaper.fm/sdk/reascript/reascripthelp.html)
documents the record-arm, monitoring and per-track anticipative flags used by this
fixture. The [official manual](https://dlx.reaper.fm/userguide/ReaperUserGuide785c.pdf)
also documents per-track exclusions and possible playback crackles under aggressive
anticipative settings. Neither specifies this private scheduler transition.

## Regression and validation

The queue contract pauses only its own idle worker via macOS `thread_suspend`,
after a completed ahead-to-live handoff; RAII resumes it even on assertion failure.
No user process or production scheduler hook is used. For each of 64/128 frames it
requests demotion, consumes 1,536 frames while that worker is unavailable, and
checks exact audio, MIDI count/offsets and a continuous DSP cursor. Re-arming cancels
an unclaimed offer. Resuming the worker must fill its cache, satisfy a retained
fan-out reader, and return cached output without misses or concurrent DSP access.

The regression fails against the old library in 0.982 s with
`unclaimed demotion must keep rendering live`. It passes after the patch in
1.292 s. The queue case now contains 22 contracts, up from 14. The existing
partial-block/nonaligned-loop contract still exercises handoff and return to
anticipation. Native integration/fault cases retain their existing assertions.

The native 64/128 routing pair was then strengthened to pause the actual source
worker across disarm for 1,536 frames inside its existing capture. This includes
real disk playback, sends, sidechains, PDC and nonaligned loops. The queue-only
patch failed this check (846/950 combined source/bus misses): the downstream bus
claimed work without future source data. Source inspection shows that waiting for
an uncached past range can also retain old cache slots. Keeping the
live dependency closure until actual source ownership changes fixes that failure;
the pair passes in 4.560 s with exact audio, zero misses, and both routes back to
ahead processing. These checks are part of the same two default matrix cases, so
no longer capture or extra engine process was added.

**All 25 cases pass in 8.58 s wall-clock** (8.549470 s inside the validator),
including the stronger native routing pair and all 22 queue contracts. The full
native GUI, libardour test targets and CoreAudio/Dummy backends build successfully.
The nine-second execution deadline and separate compilation step remain in effect. The earlier sustained CoreAudio failure is not cleared by this
patch, and the PR remains draft.

## Reproduction and retained files

```sh
python3 tools/dev/validate-native-anticipation.py . --prepare
python3 tools/dev/validate-native-anticipation.py . --case queue-contracts
python3 tools/dev/validate-native-anticipation.py .
```

`before/` and `after/` retain the queue regression summaries, logs and condensed
compiled provenance. `routing-before/` and `routing-after/` retain the native
regression exposing and then fixing the dependency-closure gap. `native/` retains the final matrix evidence. `reaper/` contains the
isolated fixture scripts and four processing traces/summaries. The launcher checks
that the user's original REAPER preferences are unchanged and terminates only its
own child. Sources use a generated WAV; the JSFX silences its outputs. No personal
project was opened, and no installed DAW or system audio service was modified.


## Final provenance

Engine revision: `afa1b683e882307743da8c926e793ad30654d32a` (the following
evidence/documentation commit has identical production and test sources).
The library SHA-256 is
`e9746d776e9cd9c6b5bde5a81dabd37c5efb1464701ef68c122cd5227078ee92`;
validator SHA-256 is
`ba8c35017e9314ea3a069b2e13be1e1bb918a4c27ed4bd7e0730a48b8a7ec7a3`.
The existing REA binary slice hash was reverified against the installed binary used
for these new REAPER captures. Preferences stayed unchanged in both hosts.
