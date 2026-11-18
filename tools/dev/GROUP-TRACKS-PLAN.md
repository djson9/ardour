# Collapsible group tracks

Implement Ableton-style arrangement groups with existing Ardour audio buses.
Keep linked-control RouteGroups independent, and leave the processing engine,
plugin hosting, latency compensation and anticipation scheduler unchanged.

## User behavior

- Select sibling tracks/buses and press Cmd+G. Create a named group bus above
  them, route their audio through it, and indent the children. Continue the
  parent color down the left edge of each child as an overhang; nested groups
  have separate inset rails, with routing/mixer columns kept aligned.
- Drag onto the middle of a group header to move tracks into it, including
  collapsed groups. Drop at its top/bottom edge to place tracks alongside it.
  Drop beside an ordinary track to join its parent, or into empty space below
  the tracks to leave all groups. Selected tracks move together; Escape cancels.
  Membership/routing changes happen only on release.
- A disclosure button folds the children without muting them or changing their
  individual visibility settings. Groups can contain other groups.
- The group has normal bus gain, mute, solo, metering and plugin controls.
- Cmd+Shift+G unwraps a selected group after confirmation, preserving its
  children and reconnecting them to the group's current output destinations.
- Deleting a group includes its descendants in the existing removal confirmation.
- Save hierarchy/collapse metadata in each route's existing extra XML; native
  audio connections and presentation order remain ordinary session state.

## Implementation sequence

1. Add a small GroupTracks model helper: metadata, ancestor traversal, ordering,
   validated bus creation, routing rollback, and ungrouping. Reject incompatible
   channel layouts or differing output destinations instead of silently changing
   the mix. Preserve MIDI connections and sends. Audio-producing MIDI tracks work;
   MIDI-only grouping is outside this first version.
2. Add editor actions, contextual shortcuts, disclosure controls and indentation.
   Make folded descendants unavailable to arrangement hit testing/selection.
   Expand ancestors when jumping to a track through search. Preserve hierarchy
   during track reordering.
3. Cover group/nested-group creation, collapse, route connections, independent
   controls, ungroup, confirmed deletion, and saved-state reload with bounded
   native checks. Each focused check has a nine-second deadline; compilation and
   app startup are measured separately.
4. Build on the existing Mac cache, exercise a disposable verification session,
   review the diff, merge to fork main, install, and reopen the user's session.

## Boundaries

Grouping and dragging do not merge clips or link child faders. Moving into a
group replaces the moved roots' audio outputs with the group inputs; moving to
the root follows the old ancestor buses' output destinations. MIDI connections
and sends remain intact. Same-parent reordering preserves manual audio outputs.
Incompatible channel layouts, descendant cycles and downstream audio feedback
are rejected before any connections change. Ungrouping removes the group's processing, so confirmation makes
the effect explicit. As with Ardour route removal, this is not undoable; keep
children and restore routing before removing the bus. Failed routing operations
restore the original connections and leave the existing tracks intact.

## Validation completed (2026-11-18)

- Native hierarchy/routing/reorder/save-reopen/ungroup: 3.26 seconds.
- Native callback audio: 3.14 seconds. Two sources sum once through a group
  processor and group gain; group mute/unmute works without changing child gains.
- GTK group workflow: under 4 seconds, including nested folding, overhang
  geometry, restored outputs, cancellation, and confirmed descendant deletion.
- Track selection/shortcuts/removal regression: 1.34 seconds.
- Native routing/monitoring/gain header regression: 0.99 seconds.
- Native macOS build and ad-hoc bundle signature verification passed.

Preparation (separate from the nine-second test budget):

```sh
python3 tools/dev/run-anticipation-probe.py . group_tracks_probe.cc --build-only
```

Focused native tests on the build host:

```sh
python3 tools/dev/test-group-tracks.py --case hierarchy
python3 tools/dev/test-group-tracks.py --case audio
```

GTK check with the prepared app already running in a disposable session:

```sh
python3 tools/dev/verify-compact-workflow.py --case groups \
  --session-path '/absolute/path/Group Tracks Verification'
```

The checks establish the group workflow and short native audio correctness, not
a new sustained real-time performance or massive-plugin-chain benchmark.
