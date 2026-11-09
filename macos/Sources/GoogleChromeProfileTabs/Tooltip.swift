import AppKit

/// The strip's own tooltip. The system shows tooltips only in the app in front, and the strip's app never is:
/// Chrome stays in front all along. It shows up under the element after a pause, and right away when the mouse
/// goes on to the next element while one is showing.
@MainActor final class Tooltip {
    private let panel = NSPanel(contentRect: .zero, styleMask: [.borderless, .nonactivatingPanel], backing: .buffered, defer: true)
    private let view = TooltipView()
    private var timer: Timer?
    /// What the timer will show: asked again for the same, the pause goes on instead of starting over.
    private var scheduled: (text: String, anchor: NSRect)?
    /// Until when the next tooltip shows without the pause: the mouse is going from one element to the next.
    private var warmUntil = Date.distantPast

    private static let delay: TimeInterval = 0.6
    private static let gap: CGFloat = 5

    init() {
        panel.isOpaque = false
        panel.backgroundColor = .clear
        panel.hasShadow = true
        panel.ignoresMouseEvents = true
        panel.isReleasedWhenClosed = false
        panel.hidesOnDeactivate = false
        panel.animationBehavior = .none
        panel.level = NSWindow.Level(rawValue: Int(CGWindowLevelForKey(.helpWindow)))
        panel.collectionBehavior = [.moveToActiveSpace, .transient, .ignoresCycle]
        panel.contentView = view
    }

    /// Shows the text under the rectangle, which is in screen coordinates.
    func show(_ text: String, below anchor: NSRect) {
        if timer != nil, let scheduled, scheduled == (text, anchor) {
            return
        }
        cancel()
        if panel.isVisible || Date() < warmUntil {
            present(text, below: anchor)
            return
        }
        let timer = Timer(timeInterval: Self.delay, repeats: false) { [weak self] _ in
            MainActor.assumeIsolated { self?.present(text, below: anchor) }
        }
        RunLoop.main.add(timer, forMode: .common)
        self.timer = timer
        scheduled = (text, anchor)
    }

    func hide() {
        cancel()
        guard panel.isVisible else { return }
        panel.orderOut(nil)
        warmUntil = Date().addingTimeInterval(0.5)
    }

    private func cancel() {
        timer?.invalidate()
        timer = nil
        scheduled = nil
    }

    /// Under the element, or above it if the screen ends below; never past the screen's sides.
    private func present(_ text: String, below anchor: NSRect) {
        cancel()
        view.text = text
        let size = view.idealSize
        var frame = NSRect(x: anchor.midX - size.width / 2, y: anchor.minY - Self.gap - size.height, width: size.width, height: size.height)
        if let screen = NSScreen.screens.first(where: { $0.frame.intersects(anchor) }) {
            let area = screen.visibleFrame
            if frame.minY < area.minY {
                frame.origin.y = anchor.maxY + Self.gap
            }
            frame.origin.x = min(max(frame.minX, area.minX + 4), area.maxX - 4 - size.width)
        }
        frame.origin = NSPoint(x: frame.minX.rounded(), y: frame.minY.rounded())
        panel.setFrame(frame, display: false)
        view.needsDisplay = true
        panel.orderFrontRegardless()
    }
}

private final class TooltipView: NSView {
    var text = ""

    private static let padding = NSSize(width: 8, height: 4)
    private static let maxTextWidth: CGFloat = 320

    override var isFlipped: Bool { true }

    private var attributes: [NSAttributedString.Key: Any] {
        let paragraph = NSMutableParagraphStyle()
        paragraph.lineBreakMode = .byTruncatingTail
        return [.font: NSFont.systemFont(ofSize: 11), .foregroundColor: Palette.activeText, .paragraphStyle: paragraph]
    }

    private var textSize: NSSize {
        let size = (text as NSString).size(withAttributes: attributes)
        return NSSize(width: min(ceil(size.width), Self.maxTextWidth), height: ceil(size.height))
    }

    var idealSize: NSSize {
        NSSize(width: textSize.width + 2 * Self.padding.width, height: textSize.height + 2 * Self.padding.height)
    }

    override func draw(_ dirtyRect: NSRect) {
        let shape = NSBezierPath(roundedRect: bounds.insetBy(dx: 0.5, dy: 0.5), xRadius: 4.5, yRadius: 4.5)
        Palette.tooltip.setFill()
        shape.fill()
        Palette.tooltipBorder.setStroke()
        shape.lineWidth = 1
        shape.stroke()
        let size = textSize
        let scale = window?.backingScaleFactor ?? 2
        let top = ((bounds.height - size.height) / 2 * scale).rounded() / scale
        (text as NSString).draw(
            with: NSRect(x: Self.padding.width, y: top, width: bounds.width - 2 * Self.padding.width, height: size.height),
            options: [.usesLineFragmentOrigin, .truncatesLastVisibleLine],
            attributes: attributes
        )
    }
}
