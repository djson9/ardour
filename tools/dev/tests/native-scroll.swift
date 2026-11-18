// SPDX-License-Identifier: GPL-2.0-or-later
// Compile once; the Python UI check supplies the verified disposable app PID.
import Cocoa
let args = CommandLine.arguments
guard args.count == 7, let pid = Int32(args[1]),
      let x = Double(args[2]), let y = Double(args[3]),
      let dx = Int32(args[4]), let dy = Int32(args[5]),
      ["gesture", "momentum", "wheel"].contains(args[6]),
      NSWorkspace.shared.frontmostApplication?.processIdentifier == pid,
      CGPreflightPostEventAccess() else {
    fputs("Expected frontmost PID, x, y, dx, dy, gesture|momentum|wheel and event-posting access\n", stderr)
    exit(1)
}
let point = CGPoint(x:x, y:y)
let windows = CGWindowListCopyWindowInfo(.optionOnScreenOnly, kCGNullWindowID) as! [[String:Any]]
guard let window = windows.first(where: { w in
    guard (w[kCGWindowOwnerPID as String] as? Int32) == pid,
          let bounds = w[kCGWindowBounds as String] as? [String:Any],
          let rect = CGRect(dictionaryRepresentation:bounds as CFDictionary) else { return false }
    return rect.contains(point)
}), let windowID = window[kCGWindowNumber as String] as? Int64 else { exit(2) }
func post(_ event:CGEvent) {
    event.location = point
    event.setIntegerValueField(.mouseEventWindowUnderMousePointer, value:windowID)
    event.setIntegerValueField(.mouseEventWindowUnderMousePointerThatCanHandleThisEvent, value:windowID)
    event.post(tap:.cghidEventTap)
}
let oldPoint = CGEvent(source:nil)!.location
CGWarpMouseCursorPosition(point)
defer { CGWarpMouseCursorPosition(oldPoint) }
post(CGEvent(mouseEventSource:nil, mouseType:.mouseMoved, mouseCursorPosition:point, mouseButton:.left)!)
Thread.sleep(forTimeInterval:0.08)
let event = CGEvent(scrollWheelEvent2Source:nil, units:args[6] == "wheel" ? .line : .pixel,
                    wheelCount:2, wheel1:dy, wheel2:dx, wheel3:0)!
// Gesture phases keep wheel-smoothing utilities from rewriting test input.
if args[6] == "momentum" {
    event.setIntegerValueField(.scrollWheelEventMomentumPhase, value:2)
} else {
    event.setIntegerValueField(.scrollWheelEventScrollPhase, value:2)
}
post(event)
Thread.sleep(forTimeInterval:0.1)
