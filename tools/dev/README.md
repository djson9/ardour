# Ardour local development

This branch adds live reload of the normal per-user `.keys` XML file, keeps its
filename stable across source commits, and adds an incremental macOS deployment
helper. It uses Ardour's existing experimental MCP HTTP surface.

## Hotkeys

On macOS, edit `~/Library/Preferences/Ardour9/ardour.keys`.
If it does not exist, copy `Ardour9.app/Contents/Resources/ardour.keys` there first.
Keep the full existing document and change the desired `Binding` element.
For example, in the `Global` group's `Press` section:

```xml
<Binding key="n" action="Main/AddTrackBus"/>
```

This opens Add Track/Bus/VCA when ordinary track selection has keyboard focus.
Text entry, including track renaming, retains its normal typing behavior. More
specific editor/mixer bindings take precedence over global ones; choose an unused
key or edit the binding in that context too. One action has one key per context.
`Primary` is Command on macOS, `Secondary` Control, `Tertiary` Shift, and `Level4`
Option. These are Ardour's existing names, not a new file format.

The watcher runs on the GUI thread every 500 ms and waits for two identical
snapshots, so ordinary edits apply in roughly 0.5–1 second. Atomic editor saves
are supported. A missing file, malformed XML, unknown action/group, or duplicate
binding leaves the last working in-memory bindings intact and logs a warning.
Removing a binding removes it from the live map; omitted groups remain unchanged.
The Keyboard Shortcuts window refreshes after a successful reload. Saving an
invalid file and then restarting does not preserve the previous in-memory copy;
fix the file before restarting.

## Numbered track types

Press N with an ordinary track selected to open Add Track/Bus/VCA. The chooser
receives focus immediately, and the displayed numbers select these types without
creating a route:

1. Audio Tracks
2. MIDI Tracks
3. Audio Busses
4. MIDI Busses
5. VCA Masters
6. Foldback Busses

The number pad works too. Enter retains the existing Add and Close behavior.
Numbers entered in the quantity/name fields remain text input. The underlying
names used for templates and saved type preferences are unchanged.

`verify-track-type-shortcuts.py --session-name 'Hotkey Verification'` checks the
six choices, number pad, input fields, route counts, and reopening focus through
MCP. On macOS it briefly activates Ardour for GTK's normal focus/navigation rules;
leave the keyboard and mouse idle during this check.

## MCP

Build with `libwebsockets` installed at configure time. Enable **MCP HTTP Server
(Experimental)** in Preferences → Control Surfaces. Its endpoint is
`http://127.0.0.1:4820/mcp`; this branch binds it to IPv4 loopback.

The built-in tools cover sessions, transport, tracks, routing, plugins, markers,
regions, and MIDI. `tools/list` is the source of truth for parameters. This branch
adds:

- `ui_get_state`: visible windows, focus widget types, and the live key/sensitivity
  for an `action` in a binding `group` (default Global).
- `ui_activate_action`: queue a named action such as `Main/AddTrackBus` on the UI
  event loop. Acknowledgement means queued; use `ui_get_state` to verify its effect.
- `ui_close_dialog`: request the normal close event for an exact visible dialog
  title; the main window is excluded.
- `ui_press_key`: queue a press/release through a visible window's GTK key handler,
  using `key` in Ardour notation and the exact `window` title. This respects text
  focus and contextual bindings. It does not synthesize global OS input.

UI mutations are asynchronous so dialogs/session lifecycle cannot deadlock the
HTTP thread. Inspection uses a bounded wait and cancels requests that time out
before starting. Normal application quit remains with the OS lifecycle.
`session_get_info` also reports `sessionPath` for safe restart checks.

For Codex CLI:

```sh
codex mcp add ardour_http --url http://127.0.0.1:4820/mcp
```

Official references:
[manual](https://manual.ardour.org/using-control-surfaces/mcp-http/) and
[upstream surface README](../../libs/surfaces/mcp_http/README.md).

## Incremental native-code loop

Configure/build and package an app once. Keep the matching packaged app on both
build host and desktop. Put this helper at the same revision on both machines.
After edits, run on the desktop:

```sh
python3 tools/dev/macos-fast-iterate.py iterate \
  --host builder@build-host \
  --source /path/to/ardour \
  --remote-app /path/to/ardour/tools/osx_packaging/Ardour9.app \
  --app "$HOME/Applications/Ardour9.app" \
  --target libardour_mcp_http --fast-compile --direct-launch \
  --restart --session '/absolute/path/Session/Session.ardour'
```

Use `--all` to seed the deployment manifest the first time and whenever new
binary targets are added. Subsequent runs reuse the discovered binary list. `--target
libardour_mcp_http` restricts a surface edit to its target/dependencies; omit it
for general changes. `--fast-compile` applies `-O0` to the MCP surface and playlist/model transactions, and `-O1` to the off-thread clip-warp adapter; audio DSP remains optimized. `--direct-launch` uses the bundle environment to
launch the development executable directly. Omit these two flags for the normal
optimized build and LaunchServices launch. The helper reports build, preparation/transfer, install,
and launch-to-MCP-ready timings. The warp adapter implementation is compiled separately from the large editor sources. Three measured MacBook build/deploy/restart cycles for adapter/optimization changes took 8.89, 8.89 and 8.25 seconds, meeting the revised under-ten-second target for these cases. Compilation still depends on which code changed
and on machine load; this is not a six-second clean build guarantee.

Only changed built dylibs/executables are copied, using compressed rsync deltas for the transfer. Their load paths are rewritten
in one operation per binary; missing Homebrew dylib dependencies are bundled.
Changed binaries are ad-hoc signed on the build host, then the application is
signed and deeply verified on the desktop. No Apple signing identity is used.
The helper saves the matching stopped session and overlaps normal shutdown with
the build. A failed build/deployment leaves the saved session on disk; reopen
the existing app after resolving the error. By default it does not force-terminate Ardour or dismiss unsaved-work dialogs. For explicitly authorized development restarts, `--terminate-saved` sends SIGTERM only to the exact app process after verifying the session path and saving it. Resource/layout changes
and LV2 bundle changes still require normal packaging. Full packaging is also
needed for a distributable release.

The deployment manifest lives under `build/fast-iterate/` and is committed only
after installation verifies. Build products, personal keys, sessions and app
bundles do not belong in git.

## Validation

With `--test` configured, build/run `run-keybinding-config-tests` using the same
runtime environment as `gtk2_ardour/ardev`. The test accepts an optional `.keys`
file to check the complete shipped/user format in addition to transactional
reload, invalid input, inherited bindings, mouse removal, pointer preservation,
and debounce cases.

Runtime verification should inspect a live changed key via MCP, send it through
`ui_press_key`, confirm the Add Track dialog, reject invalid XML without losing
the last valid binding, and restore the original file without restarting Ardour.

## Measured on 2026-11-17

Middesk: M4 Max, 36 GB RAM. Laptop: Apple Silicon macOS 15.6. These measurements
use a small saved verification session, warm configured build, and edits to
`mcp_http_ui.cc`, with `--fast-compile --direct-launch` enabled.

| Change | Seconds |
| --- | ---: |
| Hotkey file edit → live binding | 0.764 |
| Restore original hotkey → live binding | 0.544 |
| Initial optimized native-code loop | 10.320 |
| Final native-code loop, run 1 | 5.406 |
| Final native-code loop, run 2, including source edit + changed-response verification | 5.818 |
| Final native-code loop, run 3, including source edit + changed-response verification | 5.232 |

The last two trials temporarily added and then removed a diagnostic field in the
MCP response, compiled and deployed each version, and asserted the changed field
in the running app. These are real C++ edits, not no-op builds. Raw timing evidence
is in `iteration-measurements.json`.

These results meet six seconds for the tested control-code loop. They do not
promise six seconds for arbitrary C++ files, large sessions, dependency changes,
clean builds, or a heavily loaded host. Middesk showed load around 66 and 13 GB
of compressed memory during this work; one intermediate compile took 4.57 seconds
instead of roughly 1.5–1.9 seconds. Live hotkey edits avoid that variability.

Focused C++ tests and the running-app test pass, including invalid XML retention.
The original upstream build's audiographer, midi++2, evoral, ardour and temporal
suites passed. Its PBD suite had an existing Darwin RWLock assertion failure;
that unrelated test is not repaired by this branch.

## Charcoal theme and Live-style editor layout

`gtk2_ardour/themes/charcoal-ardour.colors` is an optional theme with charcoal
surfaces, flat square controls, orange transport accents, a teal playhead, and a
cyan/purple/orange palette for new tracks. It appears alongside the original
themes in Preferences → Appearance. Existing track colors are preserved.

For the macOS development bundle, save and quit Ardour, then run:

```sh
python3 tools/dev/apply-charcoal-profile.py --layout
```

The installer copies the theme, selects it, and sets `ARDOUR_LIVE_LAYOUT=1` in the
bundle's launch environment. It ad-hoc signs and verifies the changed bundle.
The source changes must be built into the app first. Launch the app again to see:

- The existing Clips/Tracks/Sources browser on the left.
- The timeline in the center and track controls on the right.
- The selected track's processor list and device controls below the timeline.
  Double-click empty space in the processor list to add a device.
- Bars/beats and range/location markers as the initial ruler set. Right-click a
  ruler to change it; subsequent choices are saved with the session.

On first use, the layout shows the browser and bottom panel and hides the left
mixer strip. Their normal visibility buttons and browser divider remain usable.
The bottom panel is shorter for empty tracks and grows to accommodate plugin
controls. Ardour's transport, editing toolbar, plugin controls, and native browser
remain Ardour interfaces; this is an Ableton-inspired arrangement, not a complete
Live UI replacement. Track selection, editing coordinates, and audio processing
use the existing implementations.

Omit `--layout` to install the colors with the traditional editor arrangement.
For other development launches, set `ARDOUR_LIVE_LAYOUT=1` in the process
environment. Unset it to restore the standard panel ordering. A mode change
resets the browser divider to a useful default. Panel visibility and ruler choices
remain ordinary saved Ardour/session preferences.

The macOS installer preserves the first `ui_config` and `Info.plist` copies in
`~/Library/Preferences/Ardour9/charcoal-profile-backup/`. With Ardour stopped,
`python3 tools/dev/apply-charcoal-profile.py --restore` restores those original
files (including preferences changed since the backup), then re-signs the app.
It does not remove the optional theme or alter tracks/audio. Use the normal
Preferences and visibility controls if you only want to adjust individual items.

Validated on macOS arm64: incremental native build and deep bundle signature
verification passed; the running app was visually checked with browser left,
track headers right, compact rulers, and the bottom device panel. Numbered track
type checks passed for all six choices, keypad input, text-entry preservation,
and unchanged route count. Live key reload passed at 0.57 seconds with malformed
XML retaining the last valid binding. The 59-color theme's 429 aliases resolve.

In Live layout, a plain Grab-tool click on an audio/MIDI region or its title
moves the playhead to the region start. Space starts playback from there; clicking
while rolling relocates without stopping. Completed drags, modifier clicks,
range selection, recording, trimming and note editing retain their existing
behavior. This change was compiled and deployed on macOS arm64; a full pointer
interaction regression check remains outstanding.

### Application logs over MCP

Call `ui_get_logs` with `{}` to read the in-process Log window messages without
opening a window. The result contains `text` with severity prefixes, `source`,
`maxBytes` (65536), and `truncated`. It keeps the latest complete lines within
that bound. It is read-only and includes engine/plugin errors that are not
written to stdout/stderr. Messages belong to this app run and do not include
macOS unified logs. Upstream retained messages have no timestamps. Restarting
clears this history. Access uses the existing bounded GUI dispatch and log mutex.

Validation: live MCP discovery/read succeeded, including a deliberately malformed
hotkey XML error followed by restored bindings. The tail buffer passed overflow,
latest-message retention, and UTF-8 line-boundary checks.

### MIDI editor activation in Live layout

A single MIDI clip selection or drag no longer opens the bottom panel or creates
its piano roll. An explicit double-click opens the bottom panel's MIDI editor.
An already visible device panel remains the selection/drag view, and this path
does not close native plugin windows. An explicitly opened MIDI editor can remain
open for its region; selecting a different region requires another double-click.
The traditional layout retains its existing behavior. Native build/deployment
passed; the piano roll was observed opening in the running app. Automated pointer
regression checks were inconclusive because the app was being used concurrently.

Selection boundary markers now also refresh when a selected region emits a
length/position or time-domain change. This covers movement and trimming without
requiring a new selection, including model changes from undo/redo or MCP. The
subscription follows the session lifetime and does not activate the piano roll.
Native build and deployment passed; interactive drag verification remains pending.

### Direct track and loop shortcuts

The `Main/AddAudioTrack` and `Main/AddMidiTrack` actions create one track directly
without the Add dialog. Audio is stereo; MIDI starts without an instrument. Both
append to the session and use the existing guarded route-creation helpers. They
are disabled without a session, in read-only sessions, and while recording.

Use these bindings in the normal per-user keys document (replace conflicting
keys/actions in the same group rather than adding duplicates):

```xml
<!-- Global / Press -->
<Binding key="Primary-t" action="Main/AddAudioTrack"/>
<Binding key="Primary-Tertiary-t" action="Main/AddMidiTrack"/>
<!-- Editor / Press -->
<Binding key="Primary-l" action="Region/loop-region"/>
<Binding key="Primary-Level4-l" action="Editor/show-editor-props"/>
```

On macOS these are Cmd+T, Cmd+Shift+T, Cmd+L, and Cmd+Option+L. Loop uses the selected
region span and starts loop playback. The bottom-panel toggle shows/hides the
current piano roll or device view. N continues to open the full Add dialog.
Native build/deployment and all four live binding/action checks passed.

### Plugin-selector keyboard and visibility controls

Enter (including keypad Enter) in search refreshes results, selects the first
match, and focuses the result list. With no matches it stays in search. Up/Down
then navigates results; Enter queues the focused plugin and invokes the existing
Insert Plugin(s) flow, including any plugins already in that queue. Modified
Enter and text editing keep their normal behavior. Mouse double-click retains
its existing queue-only action.

Right-click a result for Hide Plugin or Unhide Plugin. Show Hidden Plugins in the
same menu disables the search filter bypass and displays hidden matches so they
can be unhidden. Status is saved immediately and applies to the chosen plugin
format/ID; loaded instances are unaffected. Unhide restores Normal status.

For track navigation, the local Editor bindings use Up for
`Editor/select-prev-route` and Down for `Editor/select-next-route`. Contextual
MIDI-note, automation, mixer, text-entry, and plugin-list bindings remain separate.

### Track-title keyboard controls in the Live layout

With a track or group title focused, Enter (including keypad Enter) opens its
native rename dialog. Enter accepts and Escape cancels; both restore header
focus. Up/Down selects and focuses the adjacent visible track, bus or group in
arrangement order. Collapsed descendants are skipped, expanded nested groups
are traversed, and pinned Master is last. Navigation stops at either end.
Text fields retain their normal arrow keys, and record-armed tracks retain the
existing rename restriction.

The `track-title` case checks renaming/cancellation, focus after renaming,
repeated navigation, expanded/collapsed nested groups, and preserved hierarchy.
It retains the saved nested fixture for inspection; use a fresh disposable
session or remove the fixture before rerunning:

```sh
python3 tools/dev/verify-compact-workflow.py \
  --session-path '/path/to/Track Title Verification' --case track-title
```

The native macOS build and this case passed on 2026-11-18 (5.11 s), including
renaming a collapsed outer group while preserving its nested hierarchy.

### Selected-device details in the Live layout

The device panel shows controls for the highlighted processor only. Selecting a
plugin shows its compact parameter view; selecting an Audio Rack shows its chains
and macros through the same selection signal. Moving the pointer into those
controls keeps the highlight. Processor-list rebuilds preserve the selected
device, or select the first remaining plugin/rack when it is removed. A track
with no devices retains the add-device hint.

The track processor list and rack chain list already share `PluginSelector` and
`PluginUIWindow`. The track list additionally owns route I/O, sends, faders and
processor-window proxies; rack chains use the rack's edit/validation API.
Their owner-specific list operations remain separate.

`selected-device-ui-probe.cc` checks plugin switching, a rack, clearing selection,
pointer leave, selected-device removal, and switching to an empty track and back.
Compile it against the configured macOS build:

```sh
python3 tools/dev/prepare-selected-device-ui-probe.py /tmp/selected-device-ui-probe.dylib
```

Inject that library into a disposable session named `*Verification*` with an
`Audio` track, using `DYLD_INSERT_LIBRARIES` and
`ARDOUR_DEVICE_UI_PROBE=/tmp/selected-device-result.json`. After the GUI is ready,
create `/tmp/selected-device-result.json.trigger`. The probe loads built-in ACE
EQ/Delay fixtures, inspects the actual GTK widget tree, and writes its result
within nine seconds. It never belongs in the normal launcher or working session.
The native GTK probe passed on 2026-11-18 in 0.53 s.

### Track-selection latency in the Live layout

The compact device panel builds at most 16 controls for the selected plugin. **All controls…**
opens the existing, unlimited generic editor; the native plugin editor also
remains available from the processor list. Standalone editors and the standard
(non-Live) layout retain their full parameter lists. Compact panels skip preset
filesystem discovery because they do not display the preset toolbar. Presets are
still loaded when the full editor is explicitly opened.

On the MacBook, selecting ACE Reasonable Synth-2 (Serum + Decapitator) previously
cost 242.66–252.09 ms over ten selections. Sampling showed widget construction,
GTK realization/style lookup, and a separate first-selection AU preset-directory
scan. After bounding the compact controls and removing that unused preset scan,
ten Down-key selections measured 22.97–25.08 ms (median 23.70 ms, first selection
after restart 25.08 ms). Up measured 8.07–13.71 ms. Every measured selection met
the 100 ms budget. These are MCP key-dispatch plus GUI-thread inspection round
trips, not display-compositor or physical-key-to-photon measurements. Opening
the full editor is deliberately outside this track-selection budget.

Reproduce against a stopped session with editor focus, using the ID of the track
immediately before the target and the exact current editor title:

```sh
python3 tools/dev/measure-track-navigation.py \
  --start-track-id 5649 --window '*Hotkey Verification - Ardour'
```

The script changes track selection, checks the Up/Down bindings, prints all
samples, and fails if any measured round trip is 100 ms or longer. To include a
cold selection, run immediately after restarting Ardour. Verified on 2026-11-17:
macOS build/install, ten actual MCP arrow-key pairs, visible compact controls and
All controls button, and opening Serum's full generic editor from that button.

The playlist core rebuild/deploy/restart measured 18.05 seconds with development optimization (29.42 seconds previously). The under-ten-second measurements apply to adapter edits, not every core or header change.


### Compact arrangement workflow (Live layout)

With `ARDOUR_LIVE_LAYOUT=1`, the top bar contains BPM, meter, metronome,
count-in (Off/1/2/4 bars), session key/scale, position, play/stop/record,
Automation visibility, and the native computer-keyboard Piano toggle. The
secondary editing toolbar is hidden. Its mode, snap/grid, edit-point and nudge
keyboard bindings are removed at startup and after live key-file reloads.
Menus retain those actions. Existing session themes and device panels are reused.

Plain clicks on clips or empty arrangement space locate to the nearest visible
grid division; Snap Off uses the exact position. Completed drags and modified
clicks preserve the playhead. Right-side track headers accept Shift-click ranges,
Cmd-click toggles, and Cmd+A to select all tracks. Header-focused Delete or
Backspace uses the existing removal confirmation; Cancel preserves the tracks.
Text fields keep their own editing shortcuts.

Cmd+T creates a stereo audio track; Cmd+Shift+T creates a MIDI track. `/` opens
Find Track/Bus with eight ranked matches (exact, prefix, substring, abbreviation,
then typo distance). The first result is highlighted as the query changes.
Enter immediately jumps to it; Down chooses the second result. Further typing
resets selection to the new best result. Hidden matches are revealed on arrival.
These bindings are applied at startup and after reload without rewriting the
user's `.keys` file. The bundled GTK completion has an opt-in object-data flag
`ardour-select-first-completion`; other completion dialogs retain GTK behavior.

Quartz precise trackpad deltas now scroll continuously, including fractional
movement and momentum. Pinch zoom anchors to the gesture position. Pending
scroll/zoom changes accumulate between redraws. Native GTK event tests exercise
the handlers; a physical trackpad session is still a manual acceptance check.

Double-click an automation clip to open its bottom curve editor. Double-click
inside it to add a point, drag points to move them, or drag a segment vertically
to bend it. Delete removes a selected interior point; Escape cancels a drag.
The arrangement shows a read-only curve preview. Existing target mapping,
per-target range and inversion remain in Automation Clip Targets.

Sparse handles and outgoing bends are saved in region Extra XML. Curves compile
on edit into the existing linear playback envelope, with adaptive interpolation
and no additional audio-callback work. Undo/redo includes both representations.
If native trim/split/stretch changes the source bounds, the editor falls back to
the transformed envelope's linear knots, preserving its playback rather than
reusing stale sparse handles. This first version does not preserve sparse bend
handles across those operations. Ardour may move the last envelope point to the
last sample when loading a clip; curve handles remain intact.

The interaction was informed by Zrythm revision
`c680fbad9265cbad578d6fda1ba251766e91edaa`: `src/dsp/curve.{h,cpp}`,
`AutomationArranger.qml`, `AutomationPointView.qml`, `AutomationClipContent.qml`,
`AutomationEditorPane.qml`, and `automation_clip_canvas_renderer.cpp`.
The exponential shape/compiler here is an independent implementation; no Zrythm
code or runtime dependency was imported.

Run the standalone curve and search checks (build outside the source tree):

```sh
c++ -std=c++17 -O2 tools/dev/tests/automation-clip-curve.cc -o /tmp/ardour-curve-test
/tmp/ardour-curve-test
c++ -std=c++17 -O2 tools/dev/tests/track-name-search.cc -o /tmp/ardour-search-test
/tmp/ardour-search-test
```

For native UI checks, open a disposable session whose directory name contains
`Verification`, enable MCP on the intended port, and leave the keyboard/mouse
idle during the run. Each case enforces a nine-second deadline, creates its own
fixture tracks and confirms deletion of only those fixtures on success, except
`track-title`, which retains its nested fixture for inspection. A failed case
leaves fixtures for inspection; remove them before rerunning that case.
Never run against the working music session.

```sh
python3 tools/dev/verify-compact-workflow.py --url http://127.0.0.1:4838/mcp \
  --session-path '/absolute/path/Compact UI Verification' --case search
```

Cases: `headers`, `search`, `scroll`, `curves`. They exercise native GTK events,
not global OS key injection. On 2026-11-18, header selection/removal took 1.32 s,
search including typing/refinement took 2.43 s, fractional scroll/pinch took
1.64 s, and curve editing/read-only arrangement checks took 1.67 s. The separate recording count-in check measured 2.006 s for one bar and
4.010 s for two bars at 120 BPM (7.25 s total). Curve math checked 70 dense cases
with a worst normalized playback error of 0.00004991 in about 0.08 s. Live
shortcut reload verification took 1.73 s. These are focused UI/curve checks,
not evidence of sustained audio-engine stability.


### Native macOS scrolling regression

The first compact scrolling check injected GDK events directly and missed a
Quartz build bug: `GTK_OSX_MIN` was undefined, so the preprocessor removed the
precise-delta branch. Native trackpad/Mos events arrived as directional wheel
steps. The translator now checks Cocoa capability at runtime. Unmodified
legacy wheel events in the Live arrangement also move by 40 pixels rather than
snapping to a track boundary. Modifier-based zoom controls retain their behavior.

The `native-scroll` case crosses the missing boundary: CoreGraphics event →
Cocoa → Quartz GDK → editor. It verifies small vertical increments/reversal,
horizontal movement, momentum, header scrolling and coarse-wheel pixel steps,
plus the existing fractional accumulation and pinch checks. Its helper requires
event-posting access and checks the frontmost PID and target window before
posting. Keep the mouse/keyboard idle during this disposable-session test.
Gesture/momentum phases distinguish the input from wheel notches that tools
such as Mos can rewrite; Mos's preferences are not changed. The helper restores
the pointer afterward. Save-state checks wait for a fresh file rather than
assuming an asynchronous save finishes within a fixed delay.

```sh
swiftc tools/dev/tests/native-scroll.swift -o /tmp/ardour-native-scroll-test
python3 tools/dev/verify-compact-workflow.py \
  --session-path '/absolute/path/Compact UI Verification' \
  --case native-scroll --native-scroll-helper /tmp/ardour-native-scroll-test
```

On 2026-11-18 the final native regression passed in **5.19 seconds**, including
fixture creation and confirmed removal, under the nine-second deadline.
The macOS arm64 build, install and signature verification also passed. This
checks native event translation and exact movement, not compositor frame pacing
or a complete physical-trackpad usability session.

## Empty arrangement ranges and the loop bracket

With `ARDOUR_LIVE_LAYOUT=1`, drag empty track space to select time on the visible
tracks without switching tools. Selection grips still trim the range, and a plain
click still locates at the grid. A coalesced first pointer movement retains the
original press as its anchor. Cmd+L loops either a time selection or selected clips;
it enables Ardour's persistent loop mode without starting a stopped transport.
Stop/Play retains looping. Cmd+L toggles the existing loop off/on when the selected
range matches it, or when nothing is selected. A different selection sets and enables
a new loop. Toggling preserves transport playback and the saved loop bounds.
The middle of the joined bracket moves both endpoints;
each end grip resizes only that end. These use native marker snapping, cancellation,
and undo. The bracket follows zoom and saved loop locations, including when stopped.
Bars and the loop ruler default on, with a one-time migration for older compact
sessions; later explicit visibility changes are preserved. The Charcoal theme uses
teal selections and a translucent teal loop bracket. Log errors remain accessible
through a steady indicator in the compact bar.

```sh
python3 tools/dev/verify-compact-workflow.py \
  --session-path '/path/to/Compact UI Verification' --case loop
```

The disposable compact fixture needs its small MIDI region before bar 3 and empty
bars 3–9 on the first track, 4/4 meter, and the Bar grid. The test checks forward and
reverse selection, Cmd+L with no regions selected, ruler defaults, body/end drags,
undo/redo, Escape, zoom, click-to-locate, and unchanged regions. It also runs across
the loop boundary twice with a Stop/Play between: checking the visible bracket alone
misses Ardour's temporary-loop versus persistent-loop distinction. All checks share
a 9-second deadline; the toggle-expanded run took 4.17 seconds. The toggle checks
verify matching and empty selections both off and on, preservation of bounds and
playback, and actual passage beyond the disabled loop end. Saved bounds were also
checked after an application restart. Theme resources must be copied by normal
packaging (the incremental Mach-O delta does not include `.colors` files).

The compact arrangement places the existing summary/minimap above the bar ruler.
Its saved height, draggable divider, visibility toggle, navigation, and zoom controls
are retained; the classic layout keeps the summary below the arrangement.

## Compact position, pinned Master, and track information

With `ARDOUR_LIVE_LAYOUT=1`, the transport uses a single-row layout and displays
position as `bar . beat . sixteenth` without leading zeroes. Focus the clock to
enter a position; Enter locates and Escape restores the display. Invalid beat or
subdivision values are rejected without falling through to transport shortcuts.
BBT ticks retain their quarter-note units when converting non-quarter meters.

Master uses its existing route/view in a fixed bottom row. Its native header is
outside the scrolling header layout, and its canvas position follows the viewport.
It remains selectable, including from the canvas, while scrolling or resizing the
bottom device pane. Shift-click ordering treats Master as the last row rather than
inserting it between scrolling tracks according to its temporary canvas position.

Track headers have a colored name column, native input/output routing buttons,
In/Auto/Off monitoring, track number, mute/solo/record, numeric gain and a fader,
and a level meter. Off means playback-only monitoring (`MonitorDisk`); Auto uses
Ardour's automatic record/transport policy. Short rows hide routing details while
preserving column alignment. Master keeps its numeric gain in its short row.
These are existing routing/gain controls; this does not implement folder groups.

Additional nine-second native GTK cases:

```sh
python3 tools/dev/verify-compact-workflow.py --session-path '/path/to/Compact UI Verification' --case master-clock
python3 tools/dev/verify-compact-workflow.py --session-path '/path/to/Compact UI Verification' --case track-info
```

The clock fixture uses 120 BPM, 4/4. The first case checks text input, invalid input,
focus release, pinned header/canvas alignment while scrolling, and Master hit
selection through bottom-pane allocation changes. The second creates a disposable
track, exercises monitoring and gain against persisted/native controls, opens both
routing editors, and confirms deletion of only that test track. The pointer helper
accepts `TrackControl:track name|input`, `output`, `monitor-in`, `monitor-auto`,
`monitor-off`, and `gain`, and reports target geometry. It delivers button crossing
events and uses GtkEntry's inner text window, as real native pointer events do.

Measured on 2026-11-18 after these changes: track information 1.15 s, header
selection/deletion 1.34 s, clock/pinned Master 1.33 s, and native scrolling/pinch
5.64 s. The loop regression also passed in 3.44 s. Native scroll tests obtain
actual header geometry rather than treating the ruler-label area as a track.
The native macOS arm64 build and installed bundle signature checks passed.

## Audio racks

Right-click a track's processor list and choose **New Audio Rack**. The bottom
device panel shows its macros, chain list and selected chain's plugin list directly.
Click a chain to switch its plugin list; use the adjacent controls to add, open,
remove or reorder its plugins. Clicking the rack in the track's processor list
scrolls its controls into view. Double-click the rack for an optional floating editor.
Add chains for parallel processing; add plugins within a chain for serial
processing. Empty chains carry dry audio. Chain outputs sum, so adding a dry chain
at unity gain changes the level. Each chain has gain, mute, solo and plugin ordering.

Rename a macro by editing its name and pressing Enter. **Map…** chooses a plugin
parameter and its start/end range; reverse a mapping by making start larger than
end. A macro can drive several parameters. **Learn** maps the next touched plugin
parameter to the selected macro. Plugins without touch notifications can use the
mapping chooser. Right-click a macro to change its control type (knob, toggle or
choices), edit mappings or remove all mappings. Choice controls use labels from
the first mapped enum parameter when available, otherwise four normalized steps.

A plugin's **Learned controls…** button opens its saved parameter panel. Enable
**Learn** and touch controls in the plugin editor, or select parameters explicitly.
Right-click a learned control to remove it. Knobs, toggles, integers and enum
choices use the plugin's parameter descriptors. Learned panels and rack mappings
are separate: removing a learned panel control does not remove the plugin parameter.

This version supports audio effects with unchanged channel counts. Instruments,
MIDI-processing plugins, external sidechains, nested racks and chain-selector zones
are not supported. MIDI already present at the rack input passes through once.
Mapped parameters retain their own automation; playback automation takes precedence.
Dedicated macro automation lanes and rack preset libraries are not implemented.
Rack routes use the realtime processing path rather than anticipation workers.
Structural rack edits are disabled while recording and are not undoable.

Build with `--test` and the `dummy` backend, then run:

```sh
python3 waf build --targets=unit-test-audio-racks
python3 tools/dev/verify-audio-racks.py
```

The four native test cases cover mixing/bypass/mute/solo, MIDI preservation,
parameter mapping and removal, discrete values, reversed ranges, copy IDs,
latency alignment across block boundaries, malformed schema rejection and full
session reopening. On macOS, the runner isolates preferences and semaphore names.
`--prepare-gui` creates an isolated launchd job and native GTK interaction probe;
its generated plist is launched explicitly and must be booted out after verification.

## Chord Grid plugin reload

Chord Grid MIDI VST3 has a Reload button in its host toolbar. After installing a
new plugin build, stop transport and click Reload to preserve the current state
and replace its binary without restarting Ardour. Loading this new host button
requires one initial Ardour update/restart. See [controls, implementation and
validation](CHORD-GRID-RELOAD.md). Native 1.3.2 → 1.3.3 replacement, rollback,
playback guard and packaged-GUI button/editor tests passed on macOS arm64.

## Locating during loop playback

A locate that resumes playback uses its requested position instead of applying
Play's loop-start policy again. The loop bracket stays enabled. A seek after
its end plays forward outside the bracket; the lookahead timeline follows that
linear position too. A seek exactly to the end still wraps with the host.

Prepare the native test separately from its nine-second execution deadline:

```sh
python3 tools/dev/run-anticipation-probe.py . loop_locate_probe.cc --build-only
python3 tools/dev/test-loop-locate.py
python3 tools/dev/verify-compact-workflow.py --case loop-locate \
  --url http://127.0.0.1:4840/mcp --session-path '/absolute/path/Group Tracks Verification'
```

On 2026-11-18, the native test reproduced the old loop-start jump, then passed
in 1.77 seconds with the fixes. It verifies device-time DSP positions, active
lookahead, continued playback beyond the seek, inside/outside/end-boundary
positions, loop wrapping, and stopped locates. The native GTK mouse test passed
in 1.64 seconds. App startup and compilation are outside those test budgets.

The rack-only bottom panel is also tested independently of generic plugin panels:

```sh
python3 tools/dev/verify-rack-bottom-panel.py
```

This macOS runner opens a disposable session with isolated preferences and dummy
audio. It checks the embedded macro controls, switching between chain lists,
mouse/keyboard plugin insertion, ordering, removal, and switching to an empty
track and back. The floating editor uses the same widget; pass `--floating` to the runner to
exercise it independently.

## Files browser and editable whitelist

In the left **Files** tab, **Open Folder…** opens a directory. The existing
folder menu also supports adding bookmarks. **File Types…** edits a global,
persistent extension whitelist; defaults include audio and MIDI. Extensions
are case-insensitive and accept `wav`, `.wav`, or `*.wav`, separated by commas,
spaces, or semicolons. Cancel discards edits, Reset restores audio/MIDI defaults,
and an empty whitelist displays folders only. This filters listings; adding an
extension does not install a decoder. Folders stay navigable, expansion stays
lazy, and preview/drag-to-import use the existing clip browser paths.

While the file list has focus, Up/Down navigate rows, Left/Right expand or
collapse folders, Shift+arrows extend the selection, and Enter opens a folder or
previews a file. Home/End and Page Up/Down also navigate the list. These keys
do not trigger arrangement or computer-keyboard-piano actions in the file list.
Opening the browser or completing a folder/filter operation focuses the list.

The last opened root directory is restored at startup. Expanding a subfolder
does not change that root or Refresh's target. Missing folders report an error
instead of asserting. Hidden files and nonregular files are not listed.

```sh
python3 tools/dev/test-browser-file-filter.py
# On a configured native build host, prepare the test-only GTK dylib:
python3 tools/dev/prepare-browser-ui-probe.py artifacts/browser-ui-probe.dylib
# Inject it only into a disposable app whose session title ends in Verification.
# Set ARDOUR_BROWSER_UI_PROBE=/absolute/private/browser-test and add the dylib
# to DYLD_INSERT_LIBRARIES alongside that app's private-home interposer.
# Once the GUI is ready:
python3 tools/dev/test-browser-ui.py /absolute/private/browser-test
# Native window keyboard dispatch, including tree navigation and selection:
python3 tools/dev/test-browser-ui.py /absolute/private/browser-test --keys --pid PRIVATE_GUI_PID
# Restart that same private app, then verify saved directory/filter restoration:
python3 tools/dev/test-browser-ui.py /absolute/private/browser-test --restore
```

Each test has a hard nine-second limit; native compilation and GUI startup are
separate preparation. The GUI test exercises the folder chooser, actual tree
rows, case-insensitive audio/MIDI filtering, lazy expansion, root-preserving
refresh, invalid input, Cancel, empty filters, Reset, and startup restoration.
The injection waits for a trigger and refuses to operate on non-Verification
windows. Keep it out of production launch environments.

### Audio preview waveform and normalization

Audio selections show a waveform with playback position and duration. Click or
drag the waveform to seek (or start previewing there while stopped). **Normalize**
is on by default and remembered globally. It targets −18 LUFS with a −1 dB
true-peak ceiling, falling back to peak normalization when no usable loudness
measurement exists, including multichannel files. Amplification is capped at
+24 dB; silence stays silent. The separate audition volume knob still applies.
Only the temporary audition region receives this gain: source files and imports
are unchanged. MIDI keeps its existing synth and seek controls.

Analysis runs off the GUI/audio threads through Ardour's existing decoders and
loudness reader, in bounded blocks. Normalized playback waits for analysis;
Stop cancels pending playback. New selections cancel obsolete work, and four
recent results are cached by file identity, size, and modification time.

```sh
# On the native build host; preparation is outside test execution deadlines:
python3 tools/dev/prepare-browser-audio-test.py
python3 tools/dev/test-browser-audio.py
python3 tools/dev/prepare-browser-audio-ui-probe.py artifacts/browser-audio-ui-probe.dylib
# Inject only into the private Verification app, alongside its home interposer.
# Set ARDOUR_BROWSER_AUDIO_UI_PROBE=/absolute/private/browser-audio-test.
python3 tools/dev/test-browser-audio-ui.py /absolute/private/browser-audio-test --prepare
# After app startup:
python3 tools/dev/test-browser-audio-ui.py /absolute/private/browser-audio-test
# Restart the same private app, then:
python3 tools/dev/test-browser-audio-ui.py /absolute/private/browser-audio-test --restore
```

On 2026-11-18 the native analysis test passed in 0.88 seconds, checking equal
perceived loudness, waveform peaks/duration, true intersample-peak limiting,
silence, short clips, multichannel fallback, gain caps, invalid audio,
cancellation, and unchanged source bytes. Native GUI playback passed in 0.98
seconds: actual audition-region gain, advancing position, waveform seeking,
toggling gain without losing position, Stop, and rapid selection without late
playback. Restart preference restoration passed in 0.06 seconds; folder/filter regression
and restart checks passed in 1.38 and 0.14 seconds. Every runner
has a hard nine-second deadline; compilation and application startup are separate.
