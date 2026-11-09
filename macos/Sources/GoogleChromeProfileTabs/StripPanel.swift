import AppKit

/// The strip's window. It stands above the browser window and stays right above it in the window stack:
/// whatever covers the browser covers the strip too. It takes no focus, so a click on a tab never pulls
/// the keyboard away from the browser.
@MainActor final class StripPanel: NSPanel {
    static let height: CGFloat = 30
    static let gap: CGFloat = 3
    /// Room the strip needs above the browser window.
    static var reserve: CGFloat { height + gap }

    let tabsView = TabsView(frame: NSRect(x: 0, y: 0, width: 400, height: height))
    private var shadowStale = false

    init() {
        super.init(
            contentRect: NSRect(x: 0, y: 0, width: 400, height: Self.height),
            styleMask: [.borderless, .nonactivatingPanel],
            backing: .buffered,
            defer: true
        )
        isFloatingPanel = false
        level = .normal
        hidesOnDeactivate = false
        becomesKeyOnlyIfNeeded = true
        isOpaque = false
        backgroundColor = .clear
        hasShadow = true
        isReleasedWhenClosed = false
        animationBehavior = .none
        // The strip lives on its browser window's Space, does not show up in Mission Control as a window of
        // its own and stays out of Cmd+` cycling.
        collectionBehavior = [.moveToActiveSpace, .transient, .ignoresCycle]
        contentView = tabsView
    }

    /// The strip takes keyboard input only while a tab is being renamed. The panel does not activate the app:
    /// Chrome stays in front while the letters go into the name field.
    private var typing = false

    override var canBecomeKey: Bool { typing }

    func startTyping() {
        typing = true
        makeKey()
    }

    /// Chrome stayed in front all along and never learned the keyboard went to the strip, so it will not come
    /// back by itself. A window taken off screen stops being key, and the system gives input to the window
    /// of the app in front. Controller puts the strip back right away.
    func stopTyping() {
        typing = false
        makeFirstResponder(nil)
        if isKeyWindow {
            orderOut(nil)
        }
    }
    override var canBecomeMain: Bool { false }

    /// Puts the strip above the browser window, whose frame is in Accessibility coordinates. In live mode (the
    /// window is being dragged) the strip only moves: its place in the stack and its shadow are updated when
    /// the mouse is let go, otherwise they are rebuilt every frame and the strip twitches.
    func place(above window: CGRect, windowID: CGWindowID, live: Bool) {
        let strip = CGRect(
            x: window.minX,
            y: window.minY - Self.reserve,
            width: window.width,
            height: Self.height
        )
        let rect = AX.toCocoa(strip)
        if frame != rect {
            // A tooltip would be left hanging where the strip was.
            tabsView.hideTooltip()
            setFrame(rect, display: true)
            shadowStale = true
        }
        if !live && shadowStale {
            invalidateShadow()
            shadowStale = false
        }
        if !live || !isVisible {
            order(.above, relativeTo: Int(windowID))
        }
    }

    func hide() {
        tabsView.hideTooltip()
        if isVisible {
            orderOut(nil)
        }
    }
}
