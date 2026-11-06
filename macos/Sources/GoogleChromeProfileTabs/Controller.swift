import AppKit
import QuartzCore

struct BrowserWindow {
    let element: AXUIElement
    let id: CGWindowID
    /// Name of the profile the window belongs to.
    let profile: String
    let minimized: Bool
}

/// Ties the browser and the strip together: which window the strip sits on, what a click on a tab does,
/// where a profile's window goes.
@MainActor final class Controller {
    private let target: BrowserTarget
    private let settings: Settings
    private let profiles: ProfileStore
    private let watcher: BrowserWatcher
    private let strip = StripPanel()
    private let hotkeys = Hotkeys()

    /// The window the strip sits on.
    private var attached: BrowserWindow?
    /// The window just switched to. The browser reports its new main window a moment later, and without
    /// this the strip would jump back for an instant.
    private var sticky: (id: CGWindowID, until: Date)?
    /// A profile whose new window was asked for: when it shows up it goes where the current one is.
    private var pending: (profile: String, frame: CGRect?, known: Set<CGWindowID>, deadline: Date)?
    /// The app in front when the new window was asked for.
    private var pendingFrontmost: pid_t?
    private var profileByWindow: [CGWindowID: String] = [:]
    private var visibleTabs: [Profile] = []
    private var timers: [Timer] = []
    private var pendingPoll: Timer?
    private var roomTimer: Timer?
    private var lastMotion = Date.distantPast
    private var mouseMonitor: Any?
    private var followLink: CADisplayLink?
    private lazy var frameTicker = FrameTicker { [weak self] in self?.followFrame() }
    private var lastSummary = ""

    init(target: BrowserTarget, settings: Settings) {
        self.target = target
        self.settings = settings
        profiles = ProfileStore(target: target)
        watcher = BrowserWatcher(target: target)
    }

    func start() {
        strip.tabsView.onSelect = { [weak self] name in self?.select(name) }
        strip.tabsView.onReorder = { [weak self] names in self?.reorder(names) }
        strip.tabsView.menuProvider = { [weak self] name in self?.menu(for: name) }
        strip.tabsView.onRequestRename = { [weak self] name in self?.beginRename(name) }
        strip.tabsView.onRename = { [weak self] name, text in self?.rename(name, to: text) }
        strip.tabsView.onEditingEnded = { [weak self] in self?.editingEnded() }
        strip.tabsView.onTogglePrivacy = { [weak self] in self?.toggleNamesHidden() }
        hotkeys.onPress = { [weak self] number in self?.selectTab(number: number) }
        watcher.onEvent = { [weak self] event in self?.handle(event) }
        watcher.start()

        timers.append(repeating(every: 1) { [weak self] in self?.heartbeat() })
        // While the browser window is dragged or resized, the strip follows without lag.
        mouseMonitor = NSEvent.addGlobalMonitorForEvents(matching: [.leftMouseDragged, .leftMouseUp]) { [weak self] event in
            let released = event.type == .leftMouseUp
            MainActor.assumeIsolated { self?.mouseTracked(released: released) }
        }
        if target.isTest {
            // Checks drive the strip with a notification instead of synthetic clicks on the screen:
            // "select:Name", "rename:Name", "names".
            DistributedNotificationCenter.default().addObserver(
                forName: Notification.Name("com.max.google-chrome-profile-tabs.test"), object: nil, queue: .main
            ) { [weak self] note in
                let command = note.object as? String ?? ""
                MainActor.assumeIsolated { self?.runTestCommand(command) }
            }
        }
        updateProfiles()
        refresh()
    }

    // MARK: Events

    private func handle(_ event: BrowserWatcher.Event) {
        switch event {
        case .windowMotion(let element):
            guard let attached, AX.windowID(element) == attached.id else { return }
            lastMotion = Date()
            if NSEvent.pressedMouseButtons != 0 {
                // The window is being dragged: the frame timer leads the strip, not every notification.
                startFollowing()
            } else {
                follow(live: false)
                scheduleRoom()
            }
        case .activation, .changed:
            refresh()
        }
    }

    private func heartbeat() {
        watcher.maintain()
        updateProfiles()
        refresh()
    }

    private func updateProfiles() {
        guard watcher.isRunning else { return }
        let names = watcher.profileMenu().map(\.name)
        // The menu is empty while the browser starts: the previous list stays.
        guard !names.isEmpty, profiles.update(names: names) else { return }
        profileByWindow.removeAll()
    }

    private func mouseTracked(released: Bool) {
        guard attached != nil, watcher.isFrontmost else { return }
        if released {
            stopFollowing()
            follow(live: false)
            scheduleRoom()
        } else {
            lastMotion = Date()
            startFollowing()
        }
    }

    /// While the browser window is dragged, the strip checks it on every display frame: it moves at the
    /// display's rate, without skips and without extra work between frames.
    private func startFollowing() {
        guard followLink == nil else { return }
        let link = strip.displayLink(target: frameTicker, selector: #selector(FrameTicker.tick))
        link.add(to: .main, forMode: .common)
        followLink = link
    }

    private func stopFollowing() {
        followLink?.invalidate()
        followLink = nil
    }

    private func followFrame() {
        // The mouse release may have slipped past the mouse monitor: then finish here.
        guard NSEvent.pressedMouseButtons != 0 else {
            stopFollowing()
            follow(live: false)
            scheduleRoom()
            return
        }
        follow(live: true)
    }

    // MARK: Windows

    /// Rebuilds the picture: which windows belong to which profiles, which window the strip sits on.
    private func refresh() {
        hotkeys.setEnabled(watcher.isFrontmost)
        guard watcher.isRunning else {
            attached = nil
            strip.hide()
            showTabs(open: [])
            return
        }
        if profiles.profiles.isEmpty {
            updateProfiles()
        }
        let windows = browserWindows()
        resolvePending(windows)
        let onScreen = onScreenWindowIDs()
        let usable = windows.filter { !$0.minimized && onScreen.contains($0.id) }
        let main = watcher.mainWindow().flatMap(AX.windowID)

        if let sticky, sticky.until > Date(), let window = windows.first(where: { $0.id == sticky.id }) {
            attached = window
        } else if let window = usable.first(where: { $0.id == main }) {
            attached = window
        } else if let current = attached, let window = usable.first(where: { $0.id == current.id }) {
            attached = window
        } else {
            attached = usable.first
        }
        showTabs(open: Set(windows.map(\.profile)))
        layout()

        let summary = "profiles \(profiles.profiles.count), browser windows \(windows.map { "\($0.id):\($0.profile)" }), "
            + "on screen \(usable.count), main \(main.map(String.init) ?? "none"), attached \(attached.map { String($0.id) } ?? "none")"
        if summary != lastSummary {
            lastSummary = summary
            log.notice("\(summary, privacy: .public)")
        }
    }

    private func browserWindows() -> [BrowserWindow] {
        var alive = Set<CGWindowID>()
        let windows: [BrowserWindow] = watcher.windows().compactMap { element in
            guard let id = AX.windowID(element) else { return nil }
            alive.insert(id)
            guard let name = profile(of: element, id: id) else { return nil }
            watcher.watchClosing(element, id: id)
            return BrowserWindow(element: element, id: id, profile: name, minimized: AX.bool(element, kAXMinimizedAttribute))
        }
        profileByWindow = profileByWindow.filter { alive.contains($0.key) }
        watcher.forgetWindows(except: alive)
        return windows
    }

    /// The window's profile. Chrome ends a browser window's title with "- Google Chrome - Profile name".
    /// Web app windows have no such ending and DevTools titles start with "DevTools", so the strip leaves
    /// them alone.
    private func profile(of window: AXUIElement, id: CGWindowID) -> String? {
        if let known = profileByWindow[id] {
            return known
        }
        guard let title = AX.string(window, kAXTitleAttribute), !title.hasPrefix("DevTools") else { return nil }
        let marker = "\(target.titleName) - "
        let found: Profile?
        if let range = title.range(of: " - " + marker, options: .backwards) {
            found = profile(named: String(title[range.upperBound...]))
        } else if title.hasPrefix(marker) {
            found = profile(named: String(title.dropFirst(marker.count)))
        } else if title == target.titleName || title.hasSuffix(" - " + target.titleName) {
            // With a single profile Chrome leaves its name out of the title, and a window opened then keeps
            // no name until the page changes. Then the profile is read from the avatar button in the toolbar.
            found = profiles.profiles.count == 1 ? profiles.profiles.first : avatarProfile(of: window)
        } else {
            found = nil
        }
        guard let found else { return nil }
        profileByWindow[id] = found.name
        return found.name
    }

    private func profile(named name: String) -> Profile? {
        let list = profiles.profiles
        return list.first { $0.name == name } ?? list.first { $0.titleNames.contains(name) }
    }

    /// The profile from the avatar button. The button is found by Chrome's interface class, not by its label:
    /// a bookmark on the bar may have the same name as a profile.
    private func avatarProfile(of window: AXUIElement) -> Profile? {
        let avatar = AX.find(in: window) { element in
            AX.string(element, kAXRoleAttribute) == kAXButtonRole
                && (AX.value(element, "AXDOMClassList") as? [String])?.contains("AvatarToolbarButton") == true
        }
        return avatar.flatMap { AX.string($0, kAXDescriptionAttribute) }.flatMap(profile(named:))
    }

    private func onScreenWindowIDs() -> Set<CGWindowID> {
        guard let pid = watcher.app?.processIdentifier,
              let list = CGWindowListCopyWindowInfo([.optionOnScreenOnly, .excludeDesktopElements], kCGNullWindowID)
                as? [[String: Any]] else { return [] }
        var ids = Set<CGWindowID>()
        for info in list where (info[kCGWindowOwnerPID as String] as? Int) == Int(pid) {
            if let number = info[kCGWindowNumber as String] as? Int {
                ids.insert(CGWindowID(number))
            }
        }
        return ids
    }

    // MARK: Strip

    private func showTabs(open: Set<String>) {
        let hidden = settings.hidden
        let namesHidden = settings.namesHidden
        visibleTabs = settings.arrange(profiles.profiles).filter { !hidden.contains($0.name) }
        let tabs = visibleTabs.map { profile in
            let title = title(for: profile.name)
            let isOpen = open.contains(profile.name)
            return TabsView.Tab(
                key: profile.name,
                title: title,
                color: profile.color,
                // The account photo gives a person away as well as a name, so it goes too while names are hidden.
                picture: namesHidden ? nil : profile.picture,
                isOpen: isOpen,
                signature: "\(profile.signature)|\(title)|\(isOpen)"
            )
        }
        strip.tabsView.show(tabs, active: attached?.profile, namesHidden: namesHidden)
    }

    /// What the profile's tab says: the user's own name or Chrome's, and gibberish made of it while names
    /// are hidden.
    private func title(for name: String) -> String {
        let shown = settings.customNames[name] ?? name
        return settings.namesHidden ? Gibberish.make(for: shown, seed: settings.scrambleSeed) : shown
    }

    private func layout() {
        guard let attached, !AX.bool(attached.element, "AXFullScreen"), let frame = AX.frame(attached.element) else {
            strip.hide()
            return
        }
        strip.place(above: frame, windowID: attached.id, live: followLink != nil)
        scheduleRoom()
    }

    private func follow(live: Bool) {
        guard let attached, let frame = AX.frame(attached.element) else { return }
        strip.place(above: frame, windowID: attached.id, live: live)
    }

    private func scheduleRoom() {
        roomTimer?.invalidate()
        roomTimer = once(after: 0.35) { [weak self] in self?.makeRoom() }
    }

    /// If the window reaches the menu bar and the strip has no room, the window goes down by the strip's
    /// height and its bottom stays put. Nothing happens while the window is being dragged.
    private func makeRoom() {
        guard let attached else { return }
        guard NSEvent.pressedMouseButtons == 0, Date().timeIntervalSince(lastMotion) > 0.3 else {
            scheduleRoom()
            return
        }
        guard !AX.bool(attached.element, "AXFullScreen"),
              let frame = AX.frame(attached.element),
              let area = workArea(for: frame) else { return }
        let needed = area.minY + StripPanel.reserve
        var fixed = frame
        if frame.minY < needed - 0.5 {
            fixed.origin.y = needed
            fixed.size.height = min(frame.maxY, area.maxY) - needed
        } else if abs(frame.minY - needed) < 1, frame.maxY > area.maxY + 0.5, frame.maxY - area.maxY <= StripPanel.reserve + 1 {
            // Chrome stretches a full-screen window back after the move and its bottom ends up off screen:
            // bring the bottom back to the edge of the work area.
            fixed.size.height = area.maxY - frame.minY
        } else {
            return
        }
        guard fixed.height >= 300 else { return }
        // Height first, then the move: the window never sticks out of the screen and Chrome does not stretch it.
        AX.setSize(attached.element, fixed.size)
        AX.setPosition(attached.element, fixed.origin)
        follow(live: false)
    }

    /// The work area of the window's screen (without the menu bar and the Dock) in Accessibility coordinates.
    private func workArea(for frame: CGRect) -> CGRect? {
        func overlap(_ screen: NSScreen) -> CGFloat {
            let common = AX.fromCocoa(screen.frame).intersection(frame)
            return common.isNull ? 0 : common.width * common.height
        }
        guard let screen = NSScreen.screens.max(by: { overlap($0) < overlap($1) }), overlap(screen) > 0 else { return nil }
        return AX.fromCocoa(screen.visibleFrame)
    }

    // MARK: Switching

    private func selectTab(number: Int) {
        guard number >= 1, number <= visibleTabs.count else { return }
        select(visibleTabs[number - 1].name)
    }

    /// The profile's window takes the place of the current one and comes forward. No window: a new one opens.
    private func select(_ name: String) {
        guard watcher.isRunning else { return }
        if let attached, attached.profile == name {
            focus(attached)
            return
        }
        let frame = attached.flatMap { AX.frame($0.element) }
        let onScreen = onScreenWindowIDs()
        let candidates = browserWindows().filter { $0.profile == name }
        guard let window = candidates.first(where: { !$0.minimized && onScreen.contains($0.id) })
            ?? candidates.first(where: \.minimized)
            ?? candidates.first else {
            open(name, into: frame)
            return
        }
        if window.minimized {
            AX.set(window.element, kAXMinimizedAttribute, false)
        }
        if let frame {
            AX.setFrame(window.element, frame)
        }
        focus(window)
        sticky = (window.id, Date().addingTimeInterval(1.5))
        refresh()
    }

    private func focus(_ window: BrowserWindow) {
        AX.set(window.element, kAXMainAttribute, true)
        AX.raise(window.element)
        if let app = watcher.element {
            AX.set(app, kAXFrontmostAttribute, true)
        }
    }

    /// Chrome opens the profile's new window itself, from its Profiles menu; the strip catches the window
    /// and puts it where the current one is.
    private func open(_ name: String, into frame: CGRect?) {
        let known = Set(browserWindows().filter { $0.profile == name }.map(\.id))
        pending = (name, frame, known, Date().addingTimeInterval(20))
        pendingFrontmost = NSWorkspace.shared.frontmostApplication?.processIdentifier
        pendingPoll?.invalidate()
        pendingPoll = repeating(every: 0.1) { [weak self] in self?.refresh() }
        if !watcher.pressProfile(name) {
            log.error("no menu item for profile \(name, privacy: .public)")
            stopPending()
        }
    }

    private func resolvePending(_ windows: [BrowserWindow]) {
        guard let pending else { return }
        if let window = windows.first(where: { $0.profile == pending.profile && !pending.known.contains($0.id) && !$0.minimized }) {
            stopPending()
            if let frame = pending.frame {
                AX.setFrame(window.element, frame)
            }
            // Chrome may take a while to open the window. If the user went to another app meanwhile, the
            // window is only put in place: taking the keyboard would be a theft.
            let frontmost = NSWorkspace.shared.frontmostApplication?.processIdentifier
            if watcher.isFrontmost || frontmost == pendingFrontmost {
                focus(window)
            }
            sticky = (window.id, Date().addingTimeInterval(1.5))
        } else if pending.deadline < Date() {
            stopPending()
        }
    }

    private func stopPending() {
        pending = nil
        pendingPoll?.invalidate()
        pendingPoll = nil
    }

    // MARK: Order and menu

    private func reorder(_ names: [String]) {
        settings.reorder(visible: names, among: profiles.profiles)
        refresh()
    }

    private func setHidden(_ name: String, _ hide: Bool) {
        var hidden = settings.hidden
        if hide {
            hidden.insert(name)
        } else {
            hidden.remove(name)
        }
        settings.hidden = hidden
        refresh()
    }

    // MARK: Names

    private func beginRename(_ name: String) {
        // While names are hidden the field would show the real one.
        guard !settings.namesHidden else { return }
        strip.tabsView.beginEditing(key: name, text: settings.customNames[name] ?? name)
    }

    /// An empty name, or Chrome's own, gives the tab Chrome's name back.
    private func rename(_ name: String, to text: String) {
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        var names = settings.customNames
        names[name] = trimmed.isEmpty || trimmed == name ? nil : trimmed
        settings.customNames = names
        refresh()
    }

    /// After renaming, the keyboard goes back to Chrome unless the user went to another app.
    private func editingEnded() {
        if watcher.isFrontmost, let attached {
            focus(attached)
        }
        refresh()
    }

    private func toggleNamesHidden() {
        if !settings.namesHidden {
            settings.scrambleSeed = Int.random(in: 1...Int.max)
        }
        settings.namesHidden.toggle()
        refresh()
    }

    private func runTestCommand(_ command: String) {
        let parts = command.split(separator: ":", maxSplits: 1).map(String.init)
        switch (parts.first, parts.count > 1 ? parts[1] : nil) {
        case ("select", let name?):
            select(name)
        case ("rename", let name?):
            beginRename(name)
        case ("names", nil):
            toggleNamesHidden()
        default:
            log.error("unknown test command \(command, privacy: .public)")
        }
    }

    private func menu(for name: String?) -> NSMenu {
        let menu = NSMenu()
        if let name {
            if !settings.namesHidden {
                menu.addItem(MenuAction("Rename…") { [weak self] in self?.beginRename(name) })
                if settings.customNames[name] != nil {
                    menu.addItem(MenuAction("Reset Name to “\(name)”") { [weak self] in self?.rename(name, to: "") })
                }
            }
            menu.addItem(MenuAction("Hide Tab") { [weak self] in self?.setHidden(name, true) })
            menu.addItem(.separator())
        }
        let hidden = settings.hidden
        let hiddenProfiles = settings.arrange(profiles.profiles).filter { hidden.contains($0.name) }
        if !hiddenProfiles.isEmpty {
            let submenu = NSMenu()
            for profile in hiddenProfiles {
                submenu.addItem(MenuAction(title(for: profile.name)) { [weak self] in self?.setHidden(profile.name, false) })
            }
            let item = NSMenuItem(title: "Show Hidden Profile", action: nil, keyEquivalent: "")
            item.submenu = submenu
            menu.addItem(item)
        }
        if !target.isTest {
            menu.addItem(MenuAction("Open at Login", checked: LoginItem.isOn) { LoginItem.set(!LoginItem.isOn) })
        }
        menu.addItem(.separator())
        menu.addItem(MenuAction("Quit Google Chrome Profile Tabs") { NSApp.terminate(nil) })
        return menu
    }

    // MARK: Timers

    /// Timers run in menu and drag modes too, or the strip would freeze while a menu is open.
    private func repeating(every interval: TimeInterval, _ action: @escaping @MainActor () -> Void) -> Timer {
        let timer = Timer(timeInterval: interval, repeats: true) { _ in
            MainActor.assumeIsolated { action() }
        }
        RunLoop.main.add(timer, forMode: .common)
        return timer
    }

    private func once(after delay: TimeInterval, _ action: @escaping @MainActor () -> Void) -> Timer {
        let timer = Timer(timeInterval: delay, repeats: false) { _ in
            MainActor.assumeIsolated { action() }
        }
        RunLoop.main.add(timer, forMode: .common)
        return timer
    }
}

/// Receives display frames: CADisplayLink needs an Objective-C object as its target.
final class FrameTicker: NSObject {
    private let action: () -> Void

    init(_ action: @escaping () -> Void) {
        self.action = action
    }

    @objc func tick() {
        action()
    }
}

/// A menu item with a closure as its action.
final class MenuAction: NSMenuItem {
    private let handler: () -> Void

    init(_ title: String, checked: Bool = false, _ handler: @escaping () -> Void) {
        self.handler = handler
        super.init(title: title, action: #selector(fire), keyEquivalent: "")
        target = self
        state = checked ? .on : .off
    }

    @available(*, unavailable)
    required init(coder: NSCoder) {
        fatalError("init(coder:) is not supported")
    }

    @objc private func fire() {
        handler()
    }
}
