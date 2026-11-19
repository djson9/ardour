// Test-only native mouse input; coordinates come from the disposable GTK fixture.
import Cocoa
import CoreGraphics
let args = CommandLine.arguments.dropFirst().compactMap(Double.init)
guard args.count == 6, let app = NSRunningApplication(processIdentifier: pid_t(args[5])) else { exit(2) }
guard app.activate(options: [.activateIgnoringOtherApps]) else { exit(3) }
RunLoop.current.run(until: Date(timeIntervalSinceNow: 0.5))
guard app.isActive else { exit(3) }
let windows = CGWindowListCopyWindowInfo(.optionOnScreenOnly, kCGNullWindowID) as? [[String: Any]] ?? []
guard CGPreflightPostEventAccess(), let window = windows.first(where: { w in
    guard w[kCGWindowOwnerPID as String] as? Int == Int(args[5]),
          w[kCGWindowLayer as String] as? Int == 0,
          let bounds = w[kCGWindowBounds as String] as? [String: Any],
          let rect = CGRect(dictionaryRepresentation: bounds as CFDictionary) else { return false }
    return rect.contains(CGPoint(x: args[0], y: args[1])) && rect.contains(CGPoint(x: args[2], y: args[3]))
}), let windowID = window[kCGWindowNumber as String] as? Int64 else { exit(4) }
let oldPoint = CGEvent(source: nil)?.location
defer { if let oldPoint = oldPoint { CGWarpMouseCursorPosition(oldPoint) } }
let source = CGEventSource(stateID: .hidSystemState)
func mouse(_ type: CGEventType, _ x: Double, _ y: Double) {
    let point = CGPoint(x: x, y: y)
    CGWarpMouseCursorPosition(point)
    let event = CGEvent(mouseEventSource: source, mouseType: type, mouseCursorPosition: point, mouseButton: .left)!
    event.setIntegerValueField(.mouseEventWindowUnderMousePointer, value: windowID)
    event.setIntegerValueField(.mouseEventWindowUnderMousePointerThatCanHandleThisEvent, value: windowID)
    event.post(tap: .cghidEventTap)
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

// Let AppKit consume mouse-up before restoring the hardware cursor position.
RunLoop.current.run(until: Date(timeIntervalSinceNow: 0.5))
