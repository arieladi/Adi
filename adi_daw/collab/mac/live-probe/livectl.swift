// livectl: background control of a macOS app by pid (window list, key/mouse post, window capture)
import Foundation
import CoreGraphics
import AppKit

func die(_ s: String) -> Never { FileHandle.standardError.write((s + "\n").data(using: .utf8)!); exit(1) }
let a = CommandLine.arguments
if a.count < 2 { die("usage: livectl windows <pid> | key <pid> <keycode> [mods] | click <pid> <x> <y> | capture <wid> <out.png> | front <pid>") }

func flags(_ m: String) -> CGEventFlags {
    var f = CGEventFlags()
    for p in m.split(separator: ",") {
        switch p { case "cmd": f.insert(.maskCommand); case "shift": f.insert(.maskShift)
        case "alt": f.insert(.maskAlternate); case "ctrl": f.insert(.maskControl); default: break }
    }
    return f
}

func clearModifiers() {
    // a key event carrying modifier flags leaves the window server believing they are
    // still held (a later click arrived as a ctrl-click): release them explicitly
    let src = CGEventSource(stateID: .hidSystemState)
    for code: CGKeyCode in [59, 62, 55, 54, 56, 60, 58, 61] {
        let up = CGEvent(keyboardEventSource: src, virtualKey: code, keyDown: false)!
        up.type = .flagsChanged
        up.flags = []
        up.post(tap: .cghidEventTap)
    }
}
func requireFront(_ pid: Int32) {
    guard NSWorkspace.shared.frontmostApplication?.processIdentifier == pid else {
        FileHandle.standardError.write("refused: pid \(pid) is not frontmost\n".data(using: .utf8)!); exit(3)
    }
}
switch a[1] {
case "windows":
    let pid = Int32(a[2])!
    let list = CGWindowListCopyWindowInfo([.optionAll], kCGNullWindowID) as! [[String: Any]]
    for w in list where (w[kCGWindowOwnerPID as String] as? Int32) == pid {
        let b = w[kCGWindowBounds as String] as? [String: Any] ?? [:]
        let on = w[kCGWindowIsOnscreen as String] as? Bool ?? false
        let layer = w[kCGWindowLayer as String] as? Int ?? -1
        print("\(w[kCGWindowNumber as String]!)\ton=\(on)\tlayer=\(layer)\t\(b["X"] ?? 0),\(b["Y"] ?? 0) \(b["Width"] ?? 0)x\(b["Height"] ?? 0)\t\(w[kCGWindowName as String] ?? "")")
    }
case "key":
    let pid = Int32(a[2])!, code = CGKeyCode(UInt16(a[3])!)
    let f = a.count > 4 ? flags(a[4]) : CGEventFlags()
    let src = CGEventSource(stateID: .privateState)
    let d = CGEvent(keyboardEventSource: src, virtualKey: code, keyDown: true)!; d.flags = f
    let u = CGEvent(keyboardEventSource: src, virtualKey: code, keyDown: false)!; u.flags = f
    d.postToPid(pid); usleep(30000); u.postToPid(pid)
case "click":
    let pid = Int32(a[2])!, p = CGPoint(x: Double(a[3])!, y: Double(a[4])!)
    let src = CGEventSource(stateID: .privateState)
    let d = CGEvent(mouseEventSource: src, mouseType: .leftMouseDown, mouseCursorPosition: p, mouseButton: .left)!
    let u = CGEvent(mouseEventSource: src, mouseType: .leftMouseUp, mouseCursorPosition: p, mouseButton: .left)!
    d.postToPid(pid); usleep(50000); u.postToPid(pid)
case "hclick":
    // real HID click at global point (moves the cursor), then restores the cursor
    let p = CGPoint(x: Double(a[2])!, y: Double(a[3])!)
    let old = CGEvent(source: nil)!.location
    let src = CGEventSource(stateID: .hidSystemState)
    CGEvent(mouseEventSource: src, mouseType: .mouseMoved, mouseCursorPosition: p, mouseButton: .left)!.post(tap: .cghidEventTap)
    usleep(80000)
    let down = CGEvent(mouseEventSource: src, mouseType: .leftMouseDown, mouseCursorPosition: p, mouseButton: .left)!
    down.flags = []
    down.post(tap: .cghidEventTap)
    usleep(60000)
    let upEvent = CGEvent(mouseEventSource: src, mouseType: .leftMouseUp, mouseCursorPosition: p, mouseButton: .left)!
    upEvent.flags = []
    upEvent.post(tap: .cghidEventTap)
    usleep(80000)
    if a.count > 4 && a[4] == "stay" { break }
    CGEvent(mouseEventSource: src, mouseType: .mouseMoved, mouseCursorPosition: old, mouseButton: .left)!.post(tap: .cghidEventTap)
case "hkey":
    // real HID key press (goes to the frontmost app)
    let code = CGKeyCode(UInt16(a[2])!)
    let f = a.count > 3 ? flags(a[3]) : CGEventFlags()
    let src = CGEventSource(stateID: .hidSystemState)
    let d = CGEvent(keyboardEventSource: src, virtualKey: code, keyDown: true)!; d.flags = f
    let u = CGEvent(keyboardEventSource: src, virtualKey: code, keyDown: false)!; u.flags = f
    d.post(tap: .cghidEventTap); usleep(40000); u.post(tap: .cghidEventTap)
    if !f.isEmpty { usleep(20000); clearModifiers() }
case "frontname":
    print(NSWorkspace.shared.frontmostApplication?.localizedName ?? "")
case "hclickpid":
    // real click, but only if <pid> is frontmost at this instant
    let pid = Int32(a[2])!
    let p = CGPoint(x: Double(a[3])!, y: Double(a[4])!)
    requireFront(pid)
    let old = CGEvent(source: nil)!.location
    let src = CGEventSource(stateID: .hidSystemState)
    CGEvent(mouseEventSource: src, mouseType: .mouseMoved, mouseCursorPosition: p, mouseButton: .left)!.post(tap: .cghidEventTap)
    usleep(80000)
    requireFront(pid)
    let down = CGEvent(mouseEventSource: src, mouseType: .leftMouseDown, mouseCursorPosition: p, mouseButton: .left)!
    down.flags = []
    down.post(tap: .cghidEventTap)
    usleep(60000)
    let upEvent = CGEvent(mouseEventSource: src, mouseType: .leftMouseUp, mouseCursorPosition: p, mouseButton: .left)!
    upEvent.flags = []
    upEvent.post(tap: .cghidEventTap)
    usleep(80000)
    CGEvent(mouseEventSource: src, mouseType: .mouseMoved, mouseCursorPosition: old, mouseButton: .left)!.post(tap: .cghidEventTap)
case "hkeypid":
    let pid = Int32(a[2])!
    let code = CGKeyCode(UInt16(a[3])!)
    requireFront(pid)
    let src = CGEventSource(stateID: .hidSystemState)
    CGEvent(keyboardEventSource: src, virtualKey: code, keyDown: true)!.post(tap: .cghidEventTap); usleep(40000)
    CGEvent(keyboardEventSource: src, virtualKey: code, keyDown: false)!.post(tap: .cghidEventTap)
case "front":
    let pid = Int32(a[2])!
    NSRunningApplication(processIdentifier: pid)?.activate(options: [])
default: die("unknown")
}
