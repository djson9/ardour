# Native anticipative processing validation — 2026-11-17

The native engine is experimental and default off. Latest library revision:
`188c37370e62904dfd1b4fe5be80d7e20ee474d7`. No runtime dependency was added. The
ordinary Ardour graph, plugin instances, block size and capture path are reused.

## Passing checks

- `native-private-scratch/`: all 13 native cases pass on the latest library. These
  include 14 queue contracts, six eligibility cases, exact normal-route comparisons
  at 64/128 frames for combined disk/loop/PDC/send/sidechain processing, gain
  automation, MIDI, monitoring, and actual DiskWriter capture. The automatic
  transport case covers stop/seek, held-worker callback progress, processor/meter
  edits, controls, variable speed, loop enable, route deletion and exception recovery.
- `native/`: earlier 13-case matrix on `62b58de10b`, before the private-scratch fix.
- `feasibility-regression/`: original 21-case matrix passing on the integrated
  engine. These tests retain private fixture orchestration; they supplement the
  native matrix rather than replace it.
- `capacity/dummy-64-routes-private-scratch/`: 64 original registered routes,
  eight real delay inserts each, plus the live source/shared bus. 10,000 blocks at
  64 frames complete with zero cache misses. This caught the fixed scratch-pool
  exhaustion; queue-owned preallocation removes that hidden route-count ceiling.
- Full GUI, libardour tests and CoreAudio/Dummy backend targets build successfully.
  `preferences.png` is the actual GTK Preferences page rendered from the new GUI
  in a private test instance. The original preferences checksum remains unchanged.

## Capacity results and exact comparison

`capacity/settled-dummy-ahead` and `settled-dummy-direct` contain the final native
paired workload: eight playback routes with 64 real LV2 effects each, four effects
on the live source and four on their shared live bus. Each run covers 10,000 blocks
(13.333 seconds) at 48 kHz / 64 frames.

| Metric | Anticipated | Direct |
| --- | ---: | ---: |
| Mean whole-engine callback | 113.705 us | 859.944 us |
| Observed maximum | 13,438 us | 23,518 us |
| Backend xruns during playback | 1 | 5 |
| Missing cache ranges | 0 | 0 |

The mean reduction is 7.56x. Both exceed the nominal 1,333-us callback period at
least once: **Dummy runs do not prove hardware deadlines**. The exact comparator
records `passed: false`: the first stereo sample differs by 3.684e-20 and every
remaining sample matches bit-for-bit. No tolerance was loosened and no initial
samples were omitted. Captures contain 1,280,000 float samples / 5,120,000 bytes.

`final-dummy-ahead/direct` and `unsettled-comparison.json` preserve a failed earlier
comparison (maximum error 9.016e-5). Resetting plugins immediately before asynchronous
transport startup allowed differing numbers of silent parameter-smoothing blocks.
The harness now settles the stopped graph before measuring. The built-in delay
also injects a 1e-20 filter bias (`libs/plugins/a-delay.lv2/a-delay.c:336`), which
can retain extremely small history differences between independent stopped runs.
The direct-versus-direct repeat (`direct-repeat-comparison.json`) also differs only
in its first stereo sample, by 5.193e-20—larger than the anticipated/direct difference.
This confirms that exact independent-run equality at that bias level is not stable
in the ordinary engine either. Native same-session reference tests remain exactly equal.

The older `dummy-ahead-repeat` / `dummy-direct` captures were exactly equal at
SHA-256 `08b2f76ba68d273af17ced40f3fa046e74e5ad9db2fd5ddd90f76e8eb16e921c`.
These earlier results are retained, not substituted for the final strict comparison.

## CoreAudio startup failures and scope

`failures/` retains startup timeouts/stacks from the build Mac and the local M1 Pro
(macOS 15.6). Both native and previously successful baseline probes now stop inside
CoreAudio's AudioUnitSetProperty / IOProc setup **before Session construction**.
The local baseline was allowed its full 65-second timeout; native attempts were
terminated after stack sampling. No user's DAW, audio service or preferences were
changed. The first GUI attempt preceded the final private-home assertion in the
probe and the runner's explicit DYLD export; it never reached Session construction,
and the preferences checksum was unchanged. The console attempt and baseline used
the direct interposer environment. These are failed hardware runs, not passes.

Earlier successful short CoreAudio results live in
`../../coreaudio-evidence/`; those belong to the feasibility implementation. Native
physical interface latency and sustained hardware stability remain unverified.

## Review and remaining limits

One producer thread per eligible route, preallocated bounded queues, and complete
generation retirement on structural edits keep the implementation small. Monitor
activation drains to the existing render cursor; steady monitoring has no added
software buffer. Heavy DSP on the live path must still meet the hardware deadline.

Count-in, external sync, varispeed/reverse and offline export use the ordinary path.
External/interactive/surround routes stay live. Structural edits can briefly hold
playback and are not certified for uninterrupted recording. Arbitrary vendor-plugin
state changes, device changes, export and long teardown stress need further testing.
No hosted CI workflow exists in this fork; local builds and focused validation are
reported explicitly. Raw logs/JSON record their exact source/library hashes.
