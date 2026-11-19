# Shared plugin rows for rack chains

## Objective
Use Ardour's existing plugin rows in rack chains, with the same appearance,
bypass LED, inline controls/displays and presets, while retaining chain editing,
keyboard navigation, and moves into/out of racks.

## Minimal upstream seam
1. Add a small ProcessorEntryHost interface for the existing row-to-container
   calls. ProcessorBox implements it with its existing behavior. Keep row code
   and class names in processor_box.h/.cc; do not copy the widget.
2. Replace AudioRackUI's plugin TreeView with DnDVBox<ProcessorEntry> containing
   PluginInsertProcessorEntry objects. AudioRackUI supplies the host operations.
   Reuse the routing-display helper, preserving its existing algorithm.
3. Keep model edits in AudioRack's/Route's existing validated APIs. Route menus
   remain owned by ProcessorBox; chain menus reuse row-owned control/preset menus
   and expose chain-appropriate open, bypass, rename and remove actions.
4. Preserve Enter/arrows/Escape and stable plugin identity. Keep drag completion
   ahead of destructive widget rebuilds and use the existing ID-based transfer.
5. Make the active navigation level visible: regular processor selection focus,
   a bright/dim chain selection, and the same focus treatment on rack plugin rows.
6. Persist rack row presentation in plugin Extra state, so switching chains or
   reopening the session retains inline controls. No audio-engine/schema change.

## Validation
Build on the isolated macOS builder. Extend native GTK probes for shared row
identity, bypass isolation, inline controls/presets, save/restore, keyboard focus,
chain switching and deletion. Re-run native mouse moves between chains, reorder,
out to track, into empty chain, and cancellation in a disposable app/session.
Check ordinary track rows through the same native selected-device regression.
Commit results, create a PR against main and merge after validation.

## Upstream maintenance
The expected conflict surface is the row constructor/host pointer, the host
interface, and the routing-helper wrapper. Rendering, automation controls and
plugin display implementations stay upstream-owned. New upstream host calls
will fail compilation until both hosts supply the new behavior.

## Validation-driven corrections
The macOS GTK drag completion callback previously destroyed source state only
on successful drops, so cancellation could skip `drag-end`. Remove that guard
so every completed drag releases its state; audio/model changes still require
a successful drop. This is a small vendored-toolkit fix, outside the host seam.
Run model transfers at lower idle priority than queued GTK cleanup. Clear the
shared drag-source pointer when its list is destroyed, including when a move
replaces the source panel. These changes prevent stale source references.

Use native Cocoa snapshots in the rack probe instead of reparenting live
widgets to GTK offscreen windows. Wait for source drag completion and let
AppKit consume mouse-up before restoring the test cursor position.

## Completed validation — 2026-11-19
- Native arm64 build passes with main through `2b1a1c3fa`; all 4,870 tracked
  product/build/test source files match the isolated builder.
- All five rack engine tests pass, including identity, rollback and reopening.
- Native GTK checks pass for shared row type/style, isolated bypass, Controls
  and Presets menus, visibility serialization, Enter/arrows/Escape and text input.
- Inspected screenshots show bright focus moving from track plugins to chains
  to chain plugins; previous selections dim, and Escape restores each level.
- Native Quartz mouse moves pass between chains, within a chain, out to the
  track, into an empty chain, and Escape cancellation. Track switching and
  saving preserve the expected plugin IDs and learned controls.

Evidence is in `audio-rack-evidence/shared-row-validation.json`, the associated
build/engine/drag logs, and the three `shared-row-focus-*.png` images. Testing
used a disposable app/session with private preferences and dummy audio.
