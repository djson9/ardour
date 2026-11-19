# Plugin keyboard capture shortcut

Press **Escape** to close the focused plug-in editor. This works in native and
generic editors and returns keyboard control to Ardour when capture is active.

In a plugin window with the keyboard button, press **Cmd+K** on macOS
(**Ctrl+K** on other platforms) to toggle between plugin keyboard capture and
normal Ardour shortcuts. This invokes the same action as clicking the button,
including its icon and tooltip updates. The tooltip shows the shortcut.

The window handles Primary+K before forwarding events to a native or GTK plugin
editor, so the shortcut works in both capture modes. Key repeats and key-up do
not toggle capture. Releasing Command/Control first or leaving the window resets
the repeat latch. Other modifiers keep their existing behavior. Generic plugin
windows without this toolbar control retain their current shortcut handling.

`tools/dev/plugin_keyboard_capture_ui_probe.mm` is a test-only GUI module for an
isolated `Capture UI` session with a `Reload fixture` route. Set
`ARDOUR_CAPTURE_UI_PLUGIN` to a real Chord Grid VST3 and
`ARDOUR_CAPTURE_UI_RESULT` to the JSON report path. Use a private home, dummy
backend and packaged runtime as described in [Chord Grid reload](CHORD-GRID-RELOAD.md).
The probe queues native macOS key events, verifies capture both ways and toolbar
state, rejects repeat/key-up and modified keys, checks Command-first release,
and confirms plugin/transport state is unchanged. It also verifies generic UIs
without the capture button are excluded. Do not inject it into a user's session.
