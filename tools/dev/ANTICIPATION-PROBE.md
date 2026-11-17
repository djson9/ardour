# Anticipation feasibility probe

This standalone macOS probe links the real existing Ardour build. It exercises
`Route::bounce_process`, a deterministic stateful `Processor`, per-thread Ardour
buffers, and actual `InternalSend` / `InternalReturn` instances. It changes no
production processing path and is not a working anticipation feature.

Run from this worktree with a compatible built checkout:

```sh
python3 tools/dev/run-anticipation-probe.py ../ardour-9.10
```

The runner rejects differing library sources, records the linked library hash,
and writes its executable, compile arguments, logs and JSON under
`artifacts/anticipation-probe/`. The fixture session lives there too. It uses the
dummy backend, never attaches the fixture session to the device callback, and
does not launch or terminate the installed GUI application. A small process-local
GLib home-lookup interposer isolates macOS preferences without changing HOME;
the executable verifies isolation before initializing Ardour. The original
user-config checksum is also checked after the run.

## Experiments and results

At 64 and 128 frames, 400 blocks per size:

| Experiment | Result |
| --- | --- |
| Background route output queued, compared with direct route output | Maximum sample error 0 |
| Independent foreground route plus cached background output | Maximum mixed-output error 0 |
| Concurrent calls to one processor instance | 0 |
| Explicit render position while session position stays fixed | 51,072 versus 0 at the final sampled block |
| Naive serialized handoff from future DSP state to earlier audible position | Maximum error 0.316799 |
| Reset known processor state and replay the full preceding history | Maximum error 0 |
| Publish two real internal-send blocks, then request the earlier position from the return | Maximum error 0.0557618; only current send data remains |

The negative handoff and send cases are expected findings, and the probe fails
if they are not reproduced. These tests establish the need for state recovery
and timestamped routing storage; they do not claim either problem is solved.

## Limits

- Uses a deterministic test processor, not AU/VST3/LV2 plugins or disk playback.
- Uses the existing bounce-processing slice, not normal concurrent `Route::roll`
  or two concurrent invocations of the shared Graph.
- The foreground consumer waits in this correctness harness, so this supplies
  no hardware-deadline, latency, capacity, or underrun evidence.
- Foreground/background summation is an oracle; the separate send/return probe
  exercises real routing objects but not a working cached bus or sidechain.
- The test processor can reset its one history value; arbitrary plugins may
  not support equivalent restoration, and full-history replay is not bounded.
- Session construction and destruction complete normally on successful runs.

## Next experiments

1. Put one actual plugin through the shared route-processing slice and validate
   render-local transport callbacks and parameter/state access.
2. Add an experimental timestamped send cache with normal-route delivery, then
   test a cached source into a live bus and sidechain without consumer waits.
3. Measure REAPER's stateful-plugin monitoring transition and test the smallest
   matching handoff mechanism in Ardour.
4. Only then benchmark a paced hardware callback at 64/128 frames and real plugin
   chains; keep the behavioral-parity release requirements intact.

An early unisolated startup attempt detected a changed user-config checksum.
The before contents were not captured, so it was not blindly restored. The
runner was corrected to isolate preferences, and the successful isolated runs
verified the original configuration remained unchanged during those runs.
