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
Run model transfers at lower idle priority than GTK cleanup so the shared row
releases its drag state before a route change destroys the source panel.

Use native Cocoa snapshots in the rack probe instead of reparenting live
widgets to GTK offscreen windows. Wait for source drag completion and let
AppKit consume mouse-up before restoring the test cursor position.
