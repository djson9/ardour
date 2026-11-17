# MIDI instrument track freezing

Status: implemented and validated on 2026-11-17. Scope: the existing Ardour freeze command, in-place playback, reversible state, and saved-session reload. No new dependencies.

## Functional requirements

- Freeze a MIDI track with an internal instrument to audio, keeping the same track and routing. **Minimal:** yes.
- Stop running the instrument and baked effects; retain playback, fader, pan and metering. **Minimal:** yes.
- Unfreeze restores the original MIDI playlist and processor states, including after saving and reopening. **Minimal:** yes.
- Reject unsupported routing or disk-I/O arrangements before changing the track; cancellation leaves the original track intact. **Minimal:** yes.
- Generalize to external instruments, sidechains, cue playback and arbitrary disk-I/O placement. **Minimal:** no; reject these configurations initially.

## Existing architecture

`Track` already owns separate audio and MIDI playlists, a disk reader/writer and a freeze record. `Session::write_one_track` already renders MIDI through an instrument into audio, but currently excludes freezing from that conversion. `MidiTrack` has freeze XML serialization/restoration scaffolding while its freeze method remains a stub. `Route` configures and runs the processor chain, and the editor currently exposes the freeze command only for audio tracks.

## Proposed architecture

### Track state

Processor snapshots are captured before rendering, so cancellation can restore settings changed by automation or MIDI program changes. Unfreeze explicitly restores activation requests because skipped processors do not update their realtime activation flag. Invisible processors remain in the saved chain, retaining their IDs across reload.

- Reuse the existing MIDI playlist and `FreezeRecord`; attach a second audio playlist for frozen playback.
- Reuse processor XML snapshots for unfreeze, fixing the existing MIDI serializer's incorrect processor ID.
- Save frozen audio channel count and playlist through existing session state; restore the execution mask before playback.

### Playback and rendering

- Reuse `Session::write_one_track`, including its cancellation and source cleanup, and allow MIDI-to-audio output for freeze.
- Add an optional frozen-audio output channel count to the existing disk reader. It continues to use Ardour's butler, buffering, seeks and loop handling; it emits audio rather than MIDI while frozen.
- Add a route-owned set of processors baked into MIDI frozen playback. Skip those processors during configuration and execution, preserving their original plugin configurations for unfreeze. This avoids feeding frozen audio through a MIDI instrument's bypass mapping.
- Preserve normal fader, panner, output and meter processing. Reset baked fader gain/automation as audio freeze already does.
- Change state and configure buffers under the engine process lock; no file I/O or allocations in the audio callback.

### UI

- Reuse the existing Freeze/Unfreeze menu, progress dialog and cancellation.
- Extend frozen-track record and processor-edit guards to MIDI tracks.

## Files needed to touch

### Audio engine

The rendered file substitutes for MIDI playback on the same track. The original processor chain remains owned by that track, but baked processors are skipped. Existing XML and playlists preserve reversible state; no new database or service is introduced.

- `libs/ardour/midi_track.cc`, `ardour/midi_track.h`
  - Implement eligibility, freeze/unfreeze and reload, reusing the existing freeze record.
- `libs/ardour/session.cc`
  - Enable audio output for MIDI freezing in the existing renderer.
- `libs/ardour/route.cc`, `ardour/route.h`
  - Preserve but skip baked MIDI processors during configuration and playback.
- `libs/ardour/disk_reader.cc`, `ardour/disk_reader.h`
  - Configure and play the frozen audio through existing disk buffers.

### Editor and verification

The existing command already provides transport stopping, a progress window and a worker thread. Generalizing the dispatch avoids a second freeze workflow. Tests use a disposable MIDI instrument session and leave the user's session intact.

- `gtk2_ardour/editor.cc`, `editor_ops.cc`
  - Enable MIDI freeze dispatch and clear eligibility errors.
- `gtk2_ardour/route_ui.cc`, `processor_box.cc`, `mixer_strip.cc`, `midi_view.cc`
  - Apply frozen-state editing and recording guards to both track types.
- `tools/dev/verify-midi-freeze.py`
  - Verify render, playback, cancellation, persistence and restoration through the existing test/MCP mechanisms.

## Minimal files needed to touch

The engine and editor files above are required. No second audio track, new renderer, plugin wrapper, custom file format, or additional dependency is needed. The existing MCP surface now exposes `track_set_frozen`, frozen state and audio peak meters. Right-click pointer events and queued key delivery allow the real context menu and modal cancellation dialog to be exercised. An internal-return check also prevents the existing aux-send command from dereferencing a null bus return.

## Risks

- **Channel conversion:** a bypassed instrument can silence incoming audio; skip baked nodes entirely and test stereo output.
- **State restoration:** preserve original MIDI and processor IDs/states across reload; fail safely if frozen audio is absent.
- **Concurrency:** freeze rendering and graph changes must respect existing processing locks and cancellation boundaries.
- **Unsupported routing:** initially reject external inserts, sends/sidechains, nonstandard disk-I/O placement and active cue clips instead of baking incorrect audio.
- **CPU claims:** frozen tracks still incur audio playback/metering/routing cost; verify that baked plugins do not run, not that total CPU is zero.

## Acceptance criteria

1. Freeze a MIDI clip through ACE Reasonable Synth: non-silent stereo audio plays, original MIDI remains available, instrument/effect processing stops, fader/pan still work, and unfreeze restores original state.
2. Save/reopen while frozen, play/seek/loop, then unfreeze; compare MIDI content, routing and processor state with the pre-freeze snapshot.
3. Cancel a freeze and attempt unsupported routing; neither changes the active playlist or disables processors. Existing audio freeze still works.

UI evidence: MIDI context menu offering Freeze, frozen track with disabled processor editing and Unfreeze, and restored MIDI track. Prefer semantic assertions plus screenshots where available.

## Effort and confidence

Expected several hundred lines across the existing classes. Technical complexity 4/5; risk 4/5 because of processor channel configuration and session reload. Verification confidence target 4/5 with live instrument playback and saved-session round trips.

## Implemented verification

The disposable `MIDI Freeze Verification` session uses ACE Reasonable Synth followed by ACE Reverb after the fader, with the original audio track muted. Helpers refuse to operate outside that fixture. No user audio is required or modified.

- `verify-midi-freeze.py`: prepare a fixture once; then run `baseline`, `freeze`, `unfreeze`, `freeze`; restart the test app with the same session; run `reload`, `unfreeze`. Assert non-silent playback in each state, disabled baked plugins, identical original MIDI region data and file hashes, restored fader gain, and exact LV2 processor XML restoration. The fixture can be extended with a post-fader effect through the normal plugin selector.
- `verify-midi-freeze-cancel.py`: exercise the real GTK context menu and cancellation dialog; require unchanged original MIDI, active plugins and exact LV2 processor XML, and no surviving partial WAV files.
- `verify-midi-freeze-edges.py`: check existing audio freeze/unfreeze, rejection of an aux send to an audio bus, looping beyond the rendered range, live fader/pan changes and no additional playback xruns. Requires a non-Master audio bus in the disposable fixture.

Live semantic UI tests verify the menu and progress dialog. Screenshot capture returned a black display on the locked Mac, so pixel-level visual validation is not claimed. These checks establish correct playback and plugin bypass, not a claim of zero track CPU: disk playback, routing and meters remain active.

## Installation and rollback

Build on Middesk using its existing configuration; exercise a separate `ArdourWarpTest.app` on Jay's MacBook. Promote validated binaries into the main app only after testing, preserve its resources and Info.plist, verify the signature, and reopen the previously saved session stopped. Keep the previous app as a rollback copy. The final core edit rebuilt, transferred and relaunched in 7.2 seconds.

Final results: all three verification helpers passed against the final build, including same-session and reopened-session unfreeze. The loop helper waits for transport declick/locate completion after `rolling` becomes false before requesting unfreeze.
