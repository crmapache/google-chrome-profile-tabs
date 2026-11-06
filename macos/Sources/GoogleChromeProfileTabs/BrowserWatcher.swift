import AppKit
import ApplicationServices

/// Watches the browser process: launch and quit, activation, windows through Accessibility.
/// Reports every event through onEvent; Controller makes the decisions.
@MainActor final class BrowserWatcher {
    enum Event {
        /// A window moved or changed size.
        case windowMotion(AXUIElement)
        /// The browser came to the front or left it.
        case activation
        /// Everything else: windows opened, closed, minimized, the Space changed.
        case changed
    }

    let target: BrowserTarget
    var onEvent: ((Event) -> Void)?

    private(set) var app: NSRunningApplication?
    private(set) var element: AXUIElement?
    private var observer: AXObserver?
    private var watchedWindows = Set<CGWindowID>()
    private var profilesMenu: AXUIElement?
    /// Right after launch the browser does not take Accessibility subscriptions yet; then we try again.
    private var needsResubscribe = false

    private static let appNotifications = [
        kAXWindowCreatedNotification,
        kAXFocusedWindowChangedNotification,
        kAXMainWindowChangedNotification,
        kAXWindowMovedNotification,
        kAXWindowResizedNotification,
        kAXWindowMiniaturizedNotification,
        kAXWindowDeminiaturizedNotification,
        kAXApplicationHiddenNotification,
        kAXApplicationShownNotification,
    ]

    init(target: BrowserTarget) {
        self.target = target
    }

    var isRunning: Bool { app != nil }

    var isFrontmost: Bool {
        guard let app else { return false }
        return NSWorkspace.shared.frontmostApplication?.processIdentifier == app.processIdentifier
    }

    func start() {
        let center = NSWorkspace.shared.notificationCenter
        center.addObserver(forName: NSWorkspace.didLaunchApplicationNotification, object: nil, queue: .main) { [weak self] note in
            let launched = note.userInfo?[NSWorkspace.applicationUserInfoKey] as? NSRunningApplication
            MainActor.assumeIsolated {
                guard let self, let launched, launched.bundleIdentifier == self.target.bundleID else { return }
                self.connect(launched)
            }
        }
        center.addObserver(forName: NSWorkspace.didTerminateApplicationNotification, object: nil, queue: .main) { [weak self] note in
            let ended = note.userInfo?[NSWorkspace.applicationUserInfoKey] as? NSRunningApplication
            MainActor.assumeIsolated {
                guard let self, let ended, ended.processIdentifier == self.app?.processIdentifier else { return }
                self.disconnect()
                self.onEvent?(.changed)
            }
        }
        for name in [NSWorkspace.didActivateApplicationNotification, NSWorkspace.didDeactivateApplicationNotification] {
            center.addObserver(forName: name, object: nil, queue: .main) { [weak self] _ in
                MainActor.assumeIsolated { self?.onEvent?(.activation) }
            }
        }
        for name in [
            NSWorkspace.didHideApplicationNotification,
            NSWorkspace.didUnhideApplicationNotification,
            NSWorkspace.activeSpaceDidChangeNotification,
        ] {
            center.addObserver(forName: name, object: nil, queue: .main) { [weak self] _ in
                MainActor.assumeIsolated { self?.onEvent?(.changed) }
            }
        }
        if let running = NSRunningApplication.runningApplications(withBundleIdentifier: target.bundleID)
            .first(where: { !$0.isTerminated }) {
            connect(running)
        }
    }

    /// Called on a timer: finishes the subscription if the browser was not ready, and picks up a browser
    /// launched while the launch notification may have been missed.
    func maintain() {
        if app?.isTerminated == true {
            disconnect()
        }
        if app == nil, let running = NSRunningApplication.runningApplications(withBundleIdentifier: target.bundleID)
            .first(where: { !$0.isTerminated }) {
            connect(running)
        } else if needsResubscribe {
            subscribe()
        }
    }

    func windows() -> [AXUIElement] {
        guard let element else { return [] }
        return AX.elements(element, kAXWindowsAttribute)
    }

    func mainWindow() -> AXUIElement? {
        guard let element else { return nil }
        return AX.element(element, kAXMainWindowAttribute) ?? AX.element(element, kAXFocusedWindowAttribute)
    }

    /// Profiles from the browser's Profiles menu, in menu order. The first items are the profiles; after
    /// the separator come "Edit…" and "Add Profile…".
    func profileMenu() -> [(name: String, item: AXUIElement)] {
        if let menu = profilesMenu, let items = profileItems(in: menu) {
            return items
        }
        profilesMenu = findProfilesMenu()
        return profilesMenu.flatMap(profileItems(in:)) ?? []
    }

    /// The profile's menu item: Chrome brings the profile's window forward, or opens one if there is none.
    func pressProfile(_ name: String) -> Bool {
        guard let item = profileMenu().first(where: { $0.name == name })?.item else { return false }
        return AXUIElementPerformAction(item, kAXPressAction as CFString) == .success
    }

    private func findProfilesMenu() -> AXUIElement? {
        guard let element, let bar = AX.element(element, kAXMenuBarAttribute) else { return nil }
        let menus = AX.elements(bar, kAXChildrenAttribute).compactMap { item -> (title: String, menu: AXUIElement)? in
            guard let menu = AX.elements(item, kAXChildrenAttribute).first else { return nil }
            return (AX.string(item, kAXTitleAttribute) ?? "", menu)
        }
        if let english = menus.first(where: { $0.title == "Profiles" }) {
            return english.menu
        }
        // Chrome in another language: the profiles menu is the one listing the profile names seen in window titles.
        let open = openProfileNames()
        guard !open.isEmpty else { return nil }
        return menus.first { menu in
            guard let items = profileItems(in: menu.menu) else { return false }
            return !open.isDisjoint(with: items.map(\.name))
        }?.menu
    }

    private func profileItems(in menu: AXUIElement) -> [(name: String, item: AXUIElement)]? {
        let children = AX.elements(menu, kAXChildrenAttribute)
        guard !children.isEmpty else { return nil }
        var items: [(name: String, item: AXUIElement)] = []
        for child in children {
            guard let title = AX.string(child, kAXTitleAttribute), !title.isEmpty else { break }
            items.append((title, child))
        }
        return items
    }

    private func openProfileNames() -> Set<String> {
        let marker = " - \(target.titleName) - "
        return Set(windows().compactMap { window in
            guard let title = AX.string(window, kAXTitleAttribute),
                  let range = title.range(of: marker, options: .backwards) else { return nil }
            return String(title[range.upperBound...])
        })
    }

    /// Subscribes to a window's closing: that notification comes only from the window itself.
    func watchClosing(_ window: AXUIElement, id: CGWindowID) {
        guard let observer, !watchedWindows.contains(id) else { return }
        let result = AXObserverAddNotification(observer, window, kAXUIElementDestroyedNotification as CFString, refcon)
        if result == .success || result == .notificationAlreadyRegistered {
            watchedWindows.insert(id)
        }
    }

    func forgetWindows(except alive: Set<CGWindowID>) {
        watchedWindows.formIntersection(alive)
    }

    private var refcon: UnsafeMutableRawPointer { Unmanaged.passUnretained(self).toOpaque() }

    private func connect(_ running: NSRunningApplication) {
        disconnect()
        app = running
        let element = AXUIElementCreateApplication(running.processIdentifier)
        // A busy browser must not hang the strip: the default wait for an answer is six seconds.
        AXUIElementSetMessagingTimeout(element, 1.0)
        self.element = element
        var created: AXObserver?
        guard AXObserverCreate(running.processIdentifier, axCallback, &created) == .success, let created else {
            needsResubscribe = true
            return
        }
        observer = created
        CFRunLoopAddSource(CFRunLoopGetMain(), AXObserverGetRunLoopSource(created), .commonModes)
        subscribe()
        onEvent?(.changed)
    }

    private func subscribe() {
        guard let observer, let element else { return }
        var failed = false
        for name in Self.appNotifications {
            let result = AXObserverAddNotification(observer, element, name as CFString, refcon)
            if result != .success && result != .notificationAlreadyRegistered {
                failed = true
            }
        }
        if failed != needsResubscribe {
            log.notice("browser notifications \(failed ? "not ready, will retry" : "subscribed", privacy: .public)")
        }
        needsResubscribe = failed
    }

    private func disconnect() {
        if let observer {
            CFRunLoopRemoveSource(CFRunLoopGetMain(), AXObserverGetRunLoopSource(observer), .commonModes)
        }
        observer = nil
        element = nil
        app = nil
        profilesMenu = nil
        watchedWindows.removeAll()
        needsResubscribe = false
    }

    fileprivate func handle(_ notification: String, _ element: AXUIElement) {
        switch notification {
        case kAXWindowMovedNotification, kAXWindowResizedNotification:
            onEvent?(.windowMotion(element))
        default:
            onEvent?(.changed)
        }
    }
}

private let axCallback: AXObserverCallback = { _, element, notification, refcon in
    guard let refcon else { return }
    let watcher = Unmanaged<BrowserWatcher>.fromOpaque(refcon).takeUnretainedValue()
    let name = notification as String
    MainActor.assumeIsolated {
        watcher.handle(name, element)
    }
}
