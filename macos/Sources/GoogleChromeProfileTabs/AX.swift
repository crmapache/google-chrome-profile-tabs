import AppKit
import ApplicationServices

/// The window server's number of a window, from its Accessibility element. A private function, but every
/// window manager uses it: there is no public way to match an Accessibility window to a window on screen.
@_silgen_name("_AXUIElementGetWindow")
private func _AXUIElementGetWindow(_ element: AXUIElement, _ id: UnsafeMutablePointer<CGWindowID>) -> AXError

/// Accessibility helpers. Coordinates here are Accessibility's: the origin at the top left of the main
/// screen, Y going down.
enum AX {
    static func value(_ element: AXUIElement, _ attribute: String) -> CFTypeRef? {
        var value: CFTypeRef?
        return AXUIElementCopyAttributeValue(element, attribute as CFString, &value) == .success ? value : nil
    }

    static func string(_ element: AXUIElement, _ attribute: String) -> String? {
        value(element, attribute) as? String
    }

    static func bool(_ element: AXUIElement, _ attribute: String) -> Bool {
        value(element, attribute) as? Bool ?? false
    }

    static func element(_ element: AXUIElement, _ attribute: String) -> AXUIElement? {
        guard let value = value(element, attribute), CFGetTypeID(value) == AXUIElementGetTypeID() else { return nil }
        return (value as! AXUIElement)
    }

    static func elements(_ element: AXUIElement, _ attribute: String) -> [AXUIElement] {
        value(element, attribute) as? [AXUIElement] ?? []
    }

    static func frame(_ element: AXUIElement) -> CGRect? {
        guard let position = value(element, kAXPositionAttribute), CFGetTypeID(position) == AXValueGetTypeID(),
              let size = value(element, kAXSizeAttribute), CFGetTypeID(size) == AXValueGetTypeID() else { return nil }
        var point = CGPoint.zero
        var extent = CGSize.zero
        AXValueGetValue(position as! AXValue, .cgPoint, &point)
        AXValueGetValue(size as! AXValue, .cgSize, &extent)
        return CGRect(origin: point, size: extent)
    }

    static func setFrame(_ element: AXUIElement, _ frame: CGRect) {
        // Size first: a full-screen window moved before it is shrunk gets stretched back by Chrome, and its
        // bottom ends up off screen. A size the system clipped at the screen edge in the old place is set
        // again in the new one.
        setSize(element, frame.size)
        setPosition(element, frame.origin)
        setSize(element, frame.size)
    }

    static func setSize(_ element: AXUIElement, _ size: CGSize) {
        var size = size
        if let value = AXValueCreate(.cgSize, &size) {
            AXUIElementSetAttributeValue(element, kAXSizeAttribute as CFString, value)
        }
    }

    static func setPosition(_ element: AXUIElement, _ origin: CGPoint) {
        var point = origin
        if let value = AXValueCreate(.cgPoint, &point) {
            AXUIElementSetAttributeValue(element, kAXPositionAttribute as CFString, value)
        }
    }

    static func set(_ element: AXUIElement, _ attribute: String, _ flag: Bool) {
        AXUIElementSetAttributeValue(element, attribute as CFString, flag ? kCFBooleanTrue : kCFBooleanFalse)
    }

    static func raise(_ element: AXUIElement) {
        AXUIElementPerformAction(element, kAXRaiseAction as CFString)
    }

    /// Finds an element of the window's interface. Page content is skipped: it is huge, and the browser's
    /// buttons live outside it.
    static func find(in root: AXUIElement, limit: Int = 400, where match: (AXUIElement) -> Bool) -> AXUIElement? {
        var queue = [root]
        var visited = 0
        while !queue.isEmpty, visited < limit {
            let element = queue.removeFirst()
            visited += 1
            if match(element) {
                return element
            }
            if string(element, kAXRoleAttribute) == "AXWebArea" {
                continue
            }
            queue.append(contentsOf: elements(element, kAXChildrenAttribute))
        }
        return nil
    }

    static func windowID(_ element: AXUIElement) -> CGWindowID? {
        var id: CGWindowID = 0
        return _AXUIElementGetWindow(element, &id) == .success && id != 0 ? id : nil
    }

    /// A rectangle from Accessibility coordinates to AppKit's (origin at the bottom left, Y going up).
    static func toCocoa(_ rect: CGRect) -> NSRect {
        let height = NSScreen.screens.first?.frame.height ?? 0
        return NSRect(x: rect.minX, y: height - rect.maxY, width: rect.width, height: rect.height)
    }

    static func fromCocoa(_ rect: NSRect) -> CGRect {
        let height = NSScreen.screens.first?.frame.height ?? 0
        return CGRect(x: rect.minX, y: height - rect.maxY, width: rect.width, height: rect.height)
    }
}
