# Collapsible group tracks

Implement Ableton-style arrangement groups with existing Ardour audio buses.
Keep linked-control RouteGroups independent, and leave the processing engine,
plugin hosting, latency compensation and anticipation scheduler unchanged.

## User behavior

- Select sibling tracks/buses and press Cmd+G. Create a named group bus above
  them, route their audio through it, and indent the children.
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

This first version groups existing tracks using Cmd+G; dragging tracks into or
out of a group is a follow-up interaction. Grouping does not merge clips or link
child faders. Ungrouping removes the group's processing, so confirmation makes
the effect explicit. As with Ardour route removal, this is not undoable; keep
children and restore routing before removing the bus. Failed routing operations
restore the original connections and leave the existing tracks intact.
