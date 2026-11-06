import AppKit

/// The row of profile tabs: equal width, like JetBrains project tabs. A click switches the profile, a double
/// click renames the tab, dragging reorders, a right click opens the menu. On the right, a button hides the
/// names for screen sharing.
@MainActor final class TabsView: NSView, NSTextFieldDelegate {
    struct Tab {
        /// The profile's name in Chrome: the tab is known by it whatever it says on it.
        let key: String
        let title: String
        let color: NSColor
        let picture: NSImage?
        /// Whether the profile has an open window. Tabs without windows are paler.
        let isOpen: Bool
        /// A fingerprint to compare: did anything visible change.
        let signature: String
    }

    private enum Hover: Equatable {
        case tab(Int)
        case privacy
    }

    var onSelect: ((String) -> Void)?
    var onReorder: (([String]) -> Void)?
    var onRequestRename: ((String) -> Void)?
    var onRename: ((String, String) -> Void)?
    var onEditingEnded: (() -> Void)?
    var onTogglePrivacy: (() -> Void)?
    var menuProvider: ((String?) -> NSMenu?)?

    private var tabs: [Tab] = []
    private var active: String?
    private var namesHidden = false
    private var hovered: Hover?
    private var press: (index: Int, x: CGFloat)?
    private var pressedPrivacy = false
    /// The dragged tab: its index, where it was grabbed and where its left edge is now.
    private var drag: (index: Int, grab: CGFloat, left: CGFloat)?
    private var editor: NSTextField?
    private var editingKey: String?

    private static let buttonWidth: CGFloat = 34

    override var isFlipped: Bool { true }
    override var mouseDownCanMoveWindow: Bool { false }
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

    func show(_ tabs: [Tab], active: String?, namesHidden: Bool) {
        // The state comes every second and rarely changes: no redraw without a change.
        let same = active == self.active
            && namesHidden == self.namesHidden
            && tabs.map(\.signature) == self.tabs.map(\.signature)
        if same {
            return
        }
        if drag != nil, tabs.map(\.key) != self.tabs.map(\.key) {
            drag = nil
            press = nil
        }
        self.tabs = tabs
        self.active = active
        self.namesHidden = namesHidden
        if case .tab(let index) = hovered, index >= tabs.count {
            hovered = nil
        }
        // A field for a tab that is gone, or one that would show a real name while names are hidden, closes.
        if let editingKey, namesHidden || !tabs.contains(where: { $0.key == editingKey }) {
            endEditing(commit: false)
        }
        positionEditor()
        needsDisplay = true
    }

    // MARK: Geometry

    private var tabsWidth: CGFloat { max(0, bounds.width - Self.buttonWidth) }
    private var slot: CGFloat { tabs.isEmpty ? 0 : tabsWidth / CGFloat(tabs.count) }

    // Everything is laid out on screen pixels, not points: when the window is resized the tabs and names move
    // a pixel at a time instead of jumping by two, and edges do not shimmer.
    private var scale: CGFloat { window?.backingScaleFactor ?? 2 }

    private func pixel(_ value: CGFloat) -> CGFloat {
        (value * scale).rounded() / scale
    }

    /// The left edge of the slot for the tab at this position.
    private func edge(_ position: Int) -> CGFloat {
        pixel(CGFloat(position) * slot)
    }

    private func slotRect(_ position: Int) -> NSRect {
        NSRect(x: edge(position), y: 0, width: edge(position + 1) - edge(position), height: bounds.height)
    }

    private var buttonRect: NSRect {
        let left = edge(tabs.count)
        return NSRect(x: left, y: 0, width: bounds.width - left, height: bounds.height)
    }

    private func index(at point: NSPoint) -> Int? {
        guard !tabs.isEmpty, bounds.contains(point), point.x < tabsWidth else { return nil }
        return min(tabs.count - 1, max(0, Int(point.x / slot)))
    }

    private func location(_ event: NSEvent) -> NSPoint {
        convert(event.locationInWindow, from: nil)
    }

    // MARK: Mouse

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        trackingAreas.forEach(removeTrackingArea)
        addTrackingArea(NSTrackingArea(
            rect: .zero,
            options: [.mouseMoved, .mouseEnteredAndExited, .activeAlways, .inVisibleRect],
            owner: self
        ))
    }

    override func mouseEntered(with event: NSEvent) {
        updateCursor()
    }

    override func mouseMoved(with event: NSEvent) {
        let point = location(event)
        setHovered(buttonRect.contains(point) ? .privacy : index(at: point).map(Hover.tab))
        if let editor, editor.frame.contains(point) {
            NSCursor.iBeam.set()
        } else {
            updateCursor()
        }
    }

    override func mouseExited(with event: NSEvent) {
        setHovered(nil)
        if drag == nil {
            NSCursor.arrow.set()
        }
    }

    private func setHovered(_ hover: Hover?) {
        guard hover != hovered else { return }
        hovered = hover
        needsDisplay = true
    }

    /// A hand over the tabs and the button, a closed hand while a tab is dragged.
    private func updateCursor() {
        (drag == nil ? NSCursor.pointingHand : NSCursor.closedHand).set()
    }

    override func mouseDown(with event: NSEvent) {
        if event.modifierFlags.contains(.control) {
            if let menu = menu(for: event) {
                NSMenu.popUpContextMenu(menu, with: event, for: self)
            }
            return
        }
        let point = location(event)
        if buttonRect.contains(point) {
            pressedPrivacy = true
            return
        }
        guard let index = index(at: point) else { return }
        press = (index, point.x)
    }

    override func mouseDragged(with event: NSEvent) {
        guard let press, tabs.count > 1, editingKey == nil else { return }
        let x = location(event).x
        if drag == nil {
            guard abs(x - press.x) > 4 else { return }
            drag = (press.index, press.x - CGFloat(press.index) * slot, 0)
            updateCursor()
        }
        guard var current = drag else { return }
        current.left = min(max(x - current.grab, 0), tabsWidth - slot)
        drag = current
        needsDisplay = true
    }

    override func mouseUp(with event: NSEvent) {
        let point = location(event)
        defer {
            press = nil
            drag = nil
            pressedPrivacy = false
            needsDisplay = true
            if bounds.contains(point) {
                updateCursor()
            } else {
                NSCursor.arrow.set()
            }
        }
        if pressedPrivacy {
            if buttonRect.contains(point) {
                onTogglePrivacy?()
            }
            return
        }
        if let drag {
            let order = dropOrder(drag)
            if order != Array(tabs.indices) {
                tabs = order.map { tabs[$0] }
                onReorder?(tabs.map(\.key))
            }
            return
        }
        guard let press, index(at: point) == press.index else { return }
        let key = tabs[press.index].key
        // The first click of a double click already switched the profile; the second opens renaming.
        if event.clickCount == 2 {
            onRequestRename?(key)
        } else {
            onSelect?(key)
        }
    }

    override func menu(for event: NSEvent) -> NSMenu? {
        menuProvider?(index(at: location(event)).map { tabs[$0].key })
    }

    /// The order of the tabs if the dragged one were dropped now.
    private func dropOrder(_ drag: (index: Int, grab: CGFloat, left: CGFloat)) -> [Int] {
        let target = min(tabs.count - 1, max(0, Int((drag.left + slot / 2) / slot)))
        var order = Array(tabs.indices)
        order.remove(at: drag.index)
        order.insert(drag.index, at: target)
        return order
    }

    // MARK: Renaming

    /// A field for the new name right on the tab. Enter saves, Esc cancels, clicking elsewhere saves.
    func beginEditing(key: String, text: String) {
        endEditing(commit: false)
        guard tabs.contains(where: { $0.key == key }) else { return }
        let field = NSTextField(string: text)
        field.isBordered = false
        field.isBezeled = false
        field.drawsBackground = true
        field.backgroundColor = Palette.active
        field.textColor = Palette.activeText
        field.font = .systemFont(ofSize: 12, weight: .semibold)
        field.alignment = .center
        field.focusRingType = .none
        field.lineBreakMode = .byClipping
        field.cell?.usesSingleLineMode = true
        field.cell?.isScrollable = true
        field.delegate = self
        editor = field
        editingKey = key
        addSubview(field)
        positionEditor()
        (window as? StripPanel)?.startTyping()
        window?.makeFirstResponder(field)
        field.currentEditor()?.selectAll(nil)
        needsDisplay = true
    }

    private func positionEditor() {
        guard let editor, let key = editingKey, let index = tabs.firstIndex(where: { $0.key == key }) else { return }
        let rect = slotRect(index)
        let height: CGFloat = 18
        editor.frame = NSRect(x: rect.minX + 12, y: pixel(rect.midY - height / 2), width: max(0, rect.width - 24), height: height)
    }

    override func resizeSubviews(withOldSize oldSize: NSSize) {
        super.resizeSubviews(withOldSize: oldSize)
        positionEditor()
    }

    func control(_ control: NSControl, textView: NSTextView, doCommandBy commandSelector: Selector) -> Bool {
        switch commandSelector {
        case #selector(NSResponder.insertNewline(_:)):
            endEditing(commit: true)
            return true
        case #selector(NSResponder.cancelOperation(_:)):
            endEditing(commit: false)
            return true
        default:
            return false
        }
    }

    func controlTextDidEndEditing(_ notification: Notification) {
        endEditing(commit: true)
    }

    private func endEditing(commit: Bool) {
        guard let editor, let key = editingKey else { return }
        // Forget the field first: taking it down ends the editing and would call back here.
        self.editor = nil
        editingKey = nil
        let text = editor.stringValue
        editor.removeFromSuperview()
        (window as? StripPanel)?.stopTyping()
        if commit {
            onRename?(key, text)
        }
        onEditingEnded?()
        needsDisplay = true
    }

    // MARK: Drawing

    override func draw(_ dirtyRect: NSRect) {
        let shape = NSBezierPath(roundedRect: bounds, xRadius: 8, yRadius: 8)
        Palette.background.setFill()
        shape.fill()

        NSGraphicsContext.saveGraphicsState()
        shape.addClip()
        let order = drag.map(dropOrder) ?? Array(tabs.indices)
        for (position, index) in order.enumerated() where index != drag?.index {
            drawTab(index, in: slotRect(position))
        }
        drawSeparators(order: order)
        drawPrivacyButton(nextToActive: order.last.map { tabs[$0].key == active } ?? false)
        if let drag {
            drawTab(drag.index, in: NSRect(x: pixel(drag.left), y: 0, width: pixel(slot), height: bounds.height), lifted: true)
        }
        NSGraphicsContext.restoreGraphicsState()

        Palette.border.setStroke()
        let edge = NSBezierPath(roundedRect: bounds.insetBy(dx: 0.5, dy: 0.5), xRadius: 7.5, yRadius: 7.5)
        edge.lineWidth = 1
        edge.stroke()
    }

    private func drawSeparators(order: [Int]) {
        Palette.separator.setFill()
        for position in order.indices.dropFirst() {
            let left = order[position - 1]
            let right = order[position]
            // No line next to the selected or the dragged tab: their fill already sets them apart.
            let quiet = [left, right].contains { tabs[$0].key == active || $0 == drag?.index }
            if quiet { continue }
            NSRect(x: edge(position), y: 7, width: 1 / scale, height: bounds.height - 14).fill()
        }
    }

    private func drawPrivacyButton(nextToActive: Bool) {
        let rect = buttonRect
        if hovered == .privacy || pressedPrivacy {
            Palette.hover.setFill()
            rect.fill()
        }
        if !nextToActive && !tabs.isEmpty {
            Palette.separator.setFill()
            NSRect(x: rect.minX, y: 7, width: 1 / scale, height: bounds.height - 14).fill()
        }
        let color = namesHidden ? Palette.activeText : Palette.text.withAlphaComponent(0.65)
        let configuration = NSImage.SymbolConfiguration(pointSize: 12, weight: .medium)
            .applying(NSImage.SymbolConfiguration(paletteColors: [color]))
        guard let image = NSImage(systemSymbolName: namesHidden ? "eye.slash" : "eye", accessibilityDescription: nil)?
            .withSymbolConfiguration(configuration) else { return }
        let size = image.size
        image.draw(
            in: NSRect(x: pixel(rect.midX - size.width / 2), y: pixel(rect.midY - size.height / 2), width: size.width, height: size.height),
            from: .zero,
            operation: .sourceOver,
            fraction: 1,
            respectFlipped: true,
            hints: nil
        )
    }

    private func drawTab(_ index: Int, in rect: NSRect, lifted: Bool = false) {
        let tab = tabs[index]
        let isActive = tab.key == active
        let dark = effectiveAppearance.bestMatch(from: [.darkAqua, .aqua]) == .darkAqua

        if isActive || lifted || tab.key == editingKey {
            Palette.active.setFill()
            rect.fill()
        } else if hovered == .tab(index) && drag == nil {
            Palette.hover.setFill()
            rect.fill()
        }
        if isActive {
            Palette.accent(tab.color, dark: dark).setFill()
            NSRect(x: rect.minX, y: rect.maxY - 2, width: rect.width, height: 2).fill()
        }
        if tab.key == editingKey {
            return
        }

        let alpha: CGFloat = tab.isOpen || isActive ? 1 : 0.45
        let paragraph = NSMutableParagraphStyle()
        paragraph.lineBreakMode = .byTruncatingTail
        let attributes: [NSAttributedString.Key: Any] = [
            .font: NSFont.systemFont(ofSize: 12, weight: isActive ? .semibold : .regular),
            .foregroundColor: (isActive ? Palette.activeText : Palette.text).withAlphaComponent(alpha),
            .paragraphStyle: paragraph,
        ]
        let name = tab.title as NSString
        let size = name.size(withAttributes: attributes)
        let avatar: CGFloat = 16
        let spacing: CGFloat = 7
        let textWidth = min(ceil(size.width), max(0, rect.width - 24 - avatar - spacing))
        var x = pixel(rect.midX - (avatar + spacing + textWidth) / 2)

        drawAvatar(tab, in: NSRect(x: x, y: pixel(rect.midY - avatar / 2), width: avatar, height: avatar), alpha: alpha)
        x += avatar + spacing
        name.draw(
            with: NSRect(x: x, y: pixel(rect.midY - size.height / 2), width: textWidth, height: ceil(size.height)),
            options: [.usesLineFragmentOrigin, .truncatesLastVisibleLine],
            attributes: attributes
        )
    }

    private func drawAvatar(_ tab: Tab, in rect: NSRect, alpha: CGFloat) {
        NSGraphicsContext.saveGraphicsState()
        let circle = NSBezierPath(ovalIn: rect)
        circle.addClip()
        if let picture = tab.picture {
            picture.draw(in: rect, from: .zero, operation: .sourceOver, fraction: alpha, respectFlipped: true, hints: nil)
        } else {
            tab.color.withAlphaComponent(alpha).setFill()
            circle.fill()
            let letter = String(tab.title.prefix(1)).uppercased() as NSString
            let ink = Palette.isLight(tab.color) ? NSColor.black.withAlphaComponent(0.7) : NSColor.white.withAlphaComponent(0.92)
            let attributes: [NSAttributedString.Key: Any] = [
                .font: NSFont.systemFont(ofSize: 9.5, weight: .bold),
                .foregroundColor: ink.withAlphaComponent(ink.alphaComponent * alpha),
            ]
            let size = letter.size(withAttributes: attributes)
            letter.draw(at: NSPoint(x: pixel(rect.midX - size.width / 2), y: pixel(rect.midY - size.height / 2)), withAttributes: attributes)
        }
        NSGraphicsContext.restoreGraphicsState()
    }
}

/// Colors taken from the JetBrains project tab row (dark theme), with a light pair to match.
enum Palette {
    static let background = dynamic(dark: 0x181819, light: 0xE9EAEE)
    static let active = dynamic(dark: 0x4A4B51, light: 0xFFFFFF)
    static let hover = dynamic(dark: 0x26272B, light: 0xDCDEE3)
    static let separator = dynamic(dark: 0x393B3E, light: 0xC9CCD2)
    static let border = dynamic(dark: 0x2B2D30, light: 0xD3D5DB)
    static let text = dynamic(dark: 0xC1C4CA, light: 0x4E5157)
    static let activeText = dynamic(dark: 0xF0F1F3, light: 0x111214)

    private static func dynamic(dark: UInt32, light: UInt32) -> NSColor {
        NSColor(name: nil) { appearance in
            hex(appearance.bestMatch(from: [.darkAqua, .aqua]) == .darkAqua ? dark : light)
        }
    }

    private static func hex(_ value: UInt32) -> NSColor {
        NSColor(
            srgbRed: CGFloat((value >> 16) & 0xFF) / 255,
            green: CGFloat((value >> 8) & 0xFF) / 255,
            blue: CGFloat(value & 0xFF) / 255,
            alpha: 1
        )
    }

    /// The profile color for the line under the selected tab: a dark color on the dark strip is lifted, a
    /// light one on the light strip is darkened, so the line stays readable.
    static func accent(_ color: NSColor, dark: Bool) -> NSColor {
        guard let rgb = color.usingColorSpace(.sRGB) else { return color }
        var hue: CGFloat = 0, saturation: CGFloat = 0, brightness: CGFloat = 0, alpha: CGFloat = 0
        rgb.getHue(&hue, saturation: &saturation, brightness: &brightness, alpha: &alpha)
        let adjusted = dark ? max(brightness, 0.8) : min(brightness, 0.62)
        return NSColor(hue: hue, saturation: saturation, brightness: adjusted, alpha: 1)
    }

    static func isLight(_ color: NSColor) -> Bool {
        guard let rgb = color.usingColorSpace(.sRGB) else { return false }
        return 0.299 * rgb.redComponent + 0.587 * rgb.greenComponent + 0.114 * rgb.blueComponent > 0.6
    }
}
