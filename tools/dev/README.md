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
for general changes. `--fast-compile` applies `-O0` only to the MCP surface;
audio DSP remains optimized. `--direct-launch` uses the bundle environment to
launch the development executable directly. Omit these two flags for the normal
optimized build and LaunchServices launch. The helper reports build, preparation/transfer, install,
and launch-to-MCP-ready timings. Compilation still depends on which code changed
and on machine load; this is not a six-second clean build guarantee.

Only changed built dylibs/executables are copied. Their load paths are rewritten
in one operation per binary; missing Homebrew dylib dependencies are bundled.
Changed binaries are ad-hoc signed on the build host, then the application is
signed and deeply verified on the desktop. No Apple signing identity is used.
The helper saves the matching stopped session and overlaps normal shutdown with
the build. A failed build/deployment leaves the saved session on disk; reopen
the existing app after resolving the error. It it never
force-kills Ardour or dismisses unsaved-work dialogs. Resource/layout changes
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
