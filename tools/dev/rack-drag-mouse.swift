// Test-only native mouse input; coordinates come from the disposable GTK fixture.
import Cocoa
import CoreGraphics
let args = CommandLine.arguments.dropFirst().compactMap(Double.init)
guard args.count == 6, let app = NSRunningApplication(processIdentifier: pid_t(args[5])) else { exit(2) }
guard app.activate(options: [.activateIgnoringOtherApps]) else { exit(3) }
usleep(300_000)
guard app.isActive else { exit(3) }
let source = CGEventSource(stateID: .hidSystemState)
func mouse(_ type: CGEventType, _ x: Double, _ y: Double) {
    CGEvent(mouseEventSource: source, mouseType: type, mouseCursorPosition: CGPoint(x: x, y: y), mouseButton: .left)?.post(tap: .cghidEventTap)
}
mouse(.mouseMoved, args[0], args[1]); usleep(100_000)
mouse(.leftMouseDown, args[0], args[1]); usleep(150_000)
for i in 1...30 {
    let t = Double(i) / 30
    mouse(.leftMouseDragged, args[0] + (args[2]-args[0])*t, args[1] + (args[3]-args[1])*t)
    usleep(25_000)
}
usleep(250_000)
if args[4] != 0 {
    CGEvent(keyboardEventSource: source, virtualKey: 53, keyDown: true)?.post(tap: .cghidEventTap)
    CGEvent(keyboardEventSource: source, virtualKey: 53, keyDown: false)?.post(tap: .cghidEventTap)
    usleep(150_000)
}
mouse(.leftMouseUp, args[2], args[3])
