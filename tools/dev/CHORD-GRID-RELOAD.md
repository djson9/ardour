# Chord Grid reload in Ardour

The Chord Grid MIDI VST3 editor has a **Reload** button in Ardour's toolbar,
next to the other host controls. After installing a rebuilt plugin, stop playback
and click Reload. Ardour loads the new binary, restores the chord document and
instance settings, keeps the processor's name, bypass state, routing and chain
position, and reopens its editor. Other Chord Grid instances keep their current
code until individually reloaded. Save the session normally after editing.

The Ardour executable must be updated and launched once to add this host button.
After that, plugin updates do not require restarting Ardour. This first version
is specifically for Chord Grid MIDI VST3 with one plugin replica per insert;
it does not advertise generic hot reload for unrelated VST3 or AU plugins.

## Implementation

Ardour caches VST3 modules in `VST3PluginInfo::m`, and macOS can retain executable
images even after their last instance is removed. Reload copies the installed
bundle into a unique temporary directory and creates an independent module and
plugin info. The info retains the installed source path, so repeated reloads
pick up newly installed builds rather than reloading a previous temporary copy.
Bundle resources remain available for the lifetime of the new module and are
removed when it is released. macOS may retain code mappings until Ardour exits.

The old processor stays in the chain while the new instance is prepared. Its
complete VST3 chunk is compared after restoration before swapping. Missing or
unloadable builds and incompatible state leave the old processor in place.
Ardour's existing `Route::replace_processor` supplies chain placement and I/O
configuration rollback. The operation is deferred out of the button callback
because replacement destroys the old editor. Playback must be stopped both
before preparation and before replacement.

This code assumes Chord Grid's serialized state remains stable across the two
builds. A future state-format migration may require a version-aware comparison.

## Validation

`tools/dev/chord_grid_reload_probe.cc` loads a real 1.3.2 binary and then replaces
its installed-path fixture with 1.3.3 in the same process. It checks the factory
versions, state chunk, processor identity/name/bypass/order, continued use of the
old binary by a peer, repeated reload, missing-file rollback and playback guard.
It saves the resulting session for a separate GUI check.

Use a clean matching native build with the existing isolated probe runner:

```sh
RELOAD_OLD_PLUGIN=/path/to/1.3.2/Chord\ Grid\ MIDI.vst3 \
RELOAD_NEW_PLUGIN=/path/to/1.3.3/Chord\ Grid\ MIDI.vst3 \
CHORDGRID_MIDI_SESSIONS=/path/to/isolated/chord-sessions \
python3 tools/dev/run-anticipation-probe.py /path/to/native/build/source chord_grid_reload_probe.cc
```

The private session and preferences must remain separate from the user's live
project. Native version checks establish actual binary replacement; opening a
window or rescanning the plugin list alone is not sufficient evidence.

The separate GUI probe in `chord_grid_reload_ui_probe.cc` is injected only into a
private test process. It finds the real toolbar button, emits its click signal,
waits for the new instance and reopened editor, and checks the state again. The
user's active Ardour process is not part of either test.

## Space for host transport (macOS)

Unhandled plain Space in the Chord Grid MIDI VST3 editor invokes Ardour's
**Transport / Start/Stop** action. The native plugin view handles the key first,
so spaces in its text fields keep working. Key repeats and key-up do not toggle
transport. Modified Space and other plugin types retain their existing handling.
This also works while the plugin window captures keyboard input.

The native parent view provides this fallback; no plugin HTTP calls, synthesized
keystrokes, or changes to musical state are involved. Installing it requires one
Ardour update/relaunch. Chord Grid itself does not need a new plugin build.

`tools/dev/chord_grid_space_ui_probe.mm` runs only in a private GUI test process
with a `Space UI` fixture containing the `Reload fixture` route. Set
`ARDOUR_SPACE_UI_PLUGIN` to a real Chord Grid VST3 and `ARDOUR_SPACE_UI_RESULT` to
the JSON result path. The probe loads the plugin and sends native events through
its actual JUCE view, verifies play/stop, checks repeat/key-up, then edits the
session-name field to verify a space changes text without starting transport.
Use the same isolated home, dummy audio backend and injection/packaging process
as the reload GUI probe above. Never inject a test module into the user's session.
