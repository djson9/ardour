# Automation clips in Ardour's main timeline

## What and why

Jay wants **A** to reveal automation in the main arrangement. Curves belong to clips that can be moved, trimmed, duplicated, and undone. Each automation track can control several parameters, with independent minimum, maximum, and forward/reverse direction. This is an Ardour feature; it does not belong in the Chord Grid plugin.

## Where

Implementation branch: `jay/automation-clips`, based on `659d1d06df1b985e884407c8d520e9c0dbecfbaa`, in `/Users/davidson/workspace/ardour-anticipation-spike` on the build Mac. The fork is `djson9/ardour`. Review: [draft PR #2](https://github.com/djson9/ardour/pull/2), stacked on the anticipation integration branch.

The desktop uses the isolated **Ardour Anticipation.app**, with the **Automation Clips - Older 8 Bars** session. The original **Older 8 Bars** and **Latest Chords** sessions are preserved. The Chord Grid MIDI embedded JUCE editor remains the default, followed by Apple DLS Electric Piano 1.

## Minimal architecture

Reuse native audio-region envelopes, playlists, region editing, and undo. A small `AutomationClipSource` supplies silence without an audio file. A route marker identifies automation tracks. The source serializes with the session, so it is not treated as a missing audio file.

`AutomationClips` compiles source envelopes into the targets' existing automation lists when clips change. The ordinary Ardour automation engine handles playback and export; no new realtime cross-track parameter dispatcher or audio scheduling graph is introduced. Existing anticipation is retired before changing owned target lists.

Data flow: **clip envelope → per-target range/direction → native target automation list → existing parameter processing**.

Mappings store source track ID, target control ID, minimum, maximum, inversion, original value, and original automation state. Ranges use native target units. For volume, the dialog explicitly labels these as linear gain. An amount of 0 maps to minimum, and 1 to maximum; reverse swaps the direction.

Outside clips, the target returns to its captured original value. Muted clips do not contribute. The highest layer wins overlaps. Boundary guard points avoid unintended ramps through empty space. Linear curves are compiled at their knots; nonlinear curves are adaptively subdivided. Target assignment rejects existing automation and competing owners. Unlink restores the original automation state and value.

The editor owns the manager and uses the existing memento registry. Mapping state lives in Session Extra XML; generated target automation also saves normally. No external dependencies were introduced.

## Files and contracts

- `libs/ardour/ardour/automation_clip{,_source}.h` and `libs/ardour/automation_clip.cc`: file-free source, mappings, compilation, persistence.
- `libs/ardour/source_factory.cc` and `session_state.cc`: recognize and save the source.
- `gtk2_ardour/editor_automation_clips.cc`: A toggle, track/clip creation, target dialog.
- Editor actions, menus, bindings, and route context menu: expose the feature in the main timeline.
- Audio-region and gain-line views: normalized curve display and native mouse hit area. Drawing handler: click-to-add a point in an automation clip.
- `libs/surfaces/mcp_http/mcp_http_ui.cc`: defer action activation out of the control-surface event source so modal dialogs remain controllable.
- Native test plus `tools/dev/validate-automation-clips.py`: bounded regression runner with private preferences and process-scoped semaphores.

## Completion criteria and verification

1. A creates/reveals automation tracks and toggles visibility without disabling playback.
2. Native timeline clips support curve editing, moves, trims, copies, overlap, and undo.
3. Two targets follow independently configured ranges and inversion.
4. Save/reopen preserves source clips, mappings, and target playback; subsequent edits still propagate.
5. Existing target automation is protected, and unlink restores the original value/state.
6. Native regression finishes in less than ten seconds; compilation is a separate preparation step.

Build:

```sh
PATH=/opt/homebrew/bin:/usr/bin:/bin python3 waf build -j4 --targets=ardour-9.10.18,unit-test-automation-clips,libardour_mcp_http
python3 tools/dev/validate-automation-clips.py --prepare
python3 tools/dev/validate-automation-clips.py
```

The runner enforces a nine-second deadline. The native scenario currently passes in approximately one second. It exercises two target ranges, inversion, clip movement, point edits, clip mute, duplication/removal, front trim and undo, overlap precedence, conflicting ownership, invalid ranges, source serialization, session save/reopen, edits after reopening, unlink restoration, and refusal to overwrite existing automation.

## Risks, difficulty, and confidence

This is a prototype branch, not a production release. It reuses an audio-track container, so its track header still has audio-track controls. Clip mute is supported; track mute is not a dedicated automation bypass. Curves use native audio-envelope timing; tempo stretching and every paste/import workflow are not yet comprehensively covered. Nonlinear interpolation is approximated rather than promising sample-identical analytic curves for every plugin. Mapping ownership means edits should be made in the source clips while linked.

Confidence is stronger in the tested mapping, edit, and persistence contracts than in broad GUI workflow coverage. New source types require this Ardour build to reopen these sessions. The earlier anticipative engine's long hardware stability test remains failed; these short tests do not supersede it.

The work is primarily integration difficulty: source persistence, control lookup, mouse dispatch, and undo must agree. Reusing native regions and automation avoids a second realtime engine, but does not eliminate these integration checks.

## Implementation findings

- Session control lookup did not reliably find fresh route gain controls. Resolve IDs through route/processor ownership instead.
- Session serialization deliberately skipped non-file sources. Explicitly save the new automation source.
- Keep the source's silent wave view as the normal region mouse hit area, while hiding its zero line.
- Add a click handler for automation clips to the drawing tool; the existing handler covered MIDI/automation lanes but skipped this audio-region case.
- Dispatch modal GUI actions after leaving the MCP request source. Otherwise nested dialogs blocked subsequent UI requests.
- The model-test fixture detaches the session through the engine's locked API before destruction. Stopping the engine first races the Butler; waiting for repeated fade-outs made a short model test unreliable.

Desktop verification passed on the installed app:

- A hid and revealed the automation track through actual key events.
- Native canvas pointer events added a point to the clip; undo and redo restored the expected curves.
- Native region resize and undo restored the original duration.
- The target dialog assigned piano volume (0.15–0.30 linear gain) and reverse pan (0.20–0.80 in native pan units). Both mappings survived app restarts.
- The 1.81-second playback check observed piano audio, rising volume, and falling native pan values. The xrun counter stayed at 2 throughout that check; this is not a claim of zero xruns across the entire editing session.
- The bounded native suite passed in 0.94 seconds with private preferences and process-scoped semaphore names.

Evidence: `artifacts/chord-grid-ardour-recording/automation-ui-point-test.json` and `automation-gui-validation.json`; the build checkout stores `artifacts/automation-clips/test-result.json`.

Use **A** in the main timeline. **Track → Add Automation Track** adds another track. Right-click its header for **Automation targets…** or **New automation clip at edit point**. Use the Draw tool to add curve points and the Grab tool to move clips. **Track → Automation Clip Targets…** also opens the target editor. Target ranges and direction are independent per mapping.

