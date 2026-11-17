# Clip warp integration

The audio clip inspector exposes Beats, Tones, Texture, Re-Pitch, Complex and Complex Pro through separately installed local workers. This repository contains adapters and UI integration, not vendor DSP binaries.

`ARDOUR_WARP_ENGINE_DIR` overrides the engine directory. Otherwise it is `~/Library/Application Support/Ardour9/WarpEngines` on macOS. It contains `ArdourWarpRender`, `OriginalTextureRender`, `OriginalEngineRender`, and a `.dylib`/`.ready` pair for each mode. Worker sources and original-engine verification live in the chord-grid research repository. The current extractor targets the pinned Live 12.3.6 arm64 binary; arbitrary versions are not interchangeable. The dispatcher can be compiled with `clang++ -std=c++17 -O2 tools/dev/ardour-warp-render.cpp -o ArdourWarpRender`.

Shift-drag either edge of an enabled audio clip to stretch. The editor grid controls snapping. Left-edge stretching preserves the end position. Dragging does not change the real playlist; release makes one undoable edit and Escape cancels.

A drag owns a background renderer, an isolated playlist, and a private input cache. Workers append raw PCM while running. The renderer maps only verified available samples and prepares complete two-second track-output windows. The disk reader receives immutable windows through a three-slot handover, with a five-millisecond crossfade and no audio-callback allocation or file IO. The playlist includes neighbours, overlap rules, fades, gain and region FX; the handover precedes track processing. Reverse preserves existing semantics by reversing final rendered output, so previews near the beginning of a reversed clip still wait for the corresponding output tail.

## Validation

Run only in the disposable `Warp Verification` fixture. Helpers assert that session identity. MacBook deployment additionally verifies its exact fixture path before saving/restarting the separate `ArdourWarpTest.app`.

- `verify-clip-warp.py --texture --repitch --tones --beats`: 46 inspector checks.
- `verify-warp-drag.py --hold-seconds 8 --loop`: actual GTK Shift-edge events, active playback, preview consumption, xruns/missed windows, commit and exact undo restoration.
- `--cancel`: Escape cancellation and no project WAV creation.
- `--channels 2 --left-edge --beat-grid`: stereo, anchored end and beat-aligned length.
- `--keep-commit` leaves the result for `verify-warp-committed-audio.py`, which compares both channels against a direct original-engine render. Undo afterward.
- `verify-long-warp-modes.py`: six forward modes on a generated 60-second mono fixture.
- `verify-warp-rapid-cancel.py`: supersede requests and abort without waiting for output; checks unchanged playlist, scratch cleanup and xruns.
- `libs/ardour/test/warp_preview_test.cc` is a standalone C++17 handover test, suitable for thread/address/undefined sanitizers. It also asserts against allocation/free on the simulated audio thread.

MacBook CoreAudio validation (48 kHz, 1024 frames) passed all six long mono modes, short loop playback, long stereo Texture, both edges, beat snapping, reverse, cancellation and rapid request supersession with zero reported xruns or missed preview windows. Stereo committed output matched direct renders exactly. These are synthetic fixtures, not a guarantee for arbitrary sessions or hardware. Following input caching, one long stereo test measured initial/subsequent handovers of approximately 300/58 ms. Initial source preparation and reverse-tail readiness can take longer.

## Outstanding

- Test overlap/region effects, trimmed ancestry, and additional stressed-session cases.
- Replace `.ready` marker checks with a versioned capability handshake.
- Improve initial preview latency. A universal latency guarantee is not established.
- The requested six-second full iteration loop is not met: the latest incremental compile alone took 7.565 seconds.
- Final installation into the main user app is separate from validation in the dedicated test app.
