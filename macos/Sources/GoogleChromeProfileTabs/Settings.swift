import Foundation
import ServiceManagement

/// What the user set up in the strip: tab order, hidden profiles, own names.
@MainActor final class Settings {
    private let defaults: UserDefaults

    init(isTest: Bool) {
        // The test copy keeps its own settings so checks never shuffle the real tabs.
        defaults = isTest ? UserDefaults(suiteName: "com.max.google-chrome-profile-tabs.test") ?? .standard : .standard
    }

    /// Profile names in tab order. Profiles missing here go to the end in Chrome's order.
    var order: [String] {
        get { defaults.stringArray(forKey: "order") ?? [] }
        set { defaults.set(newValue, forKey: "order") }
    }

    var hidden: Set<String> {
        get { Set(defaults.stringArray(forKey: "hidden") ?? []) }
        set { defaults.set(newValue.sorted(), forKey: "hidden") }
    }

    /// Profiles closed and folded into icons at the right end of the strip.
    var collapsed: Set<String> {
        get { Set(defaults.stringArray(forKey: "collapsed") ?? []) }
        set { defaults.set(newValue.sorted(), forKey: "collapsed") }
    }

    /// Own tab names: profile name in Chrome -> what the tab says.
    var customNames: [String: String] {
        get { defaults.dictionary(forKey: "customNames") as? [String: String] ?? [:] }
        set { defaults.set(newValue, forKey: "customNames") }
    }

    /// Names are hidden for screen sharing. Survives a restart so they never pop up mid-share.
    var namesHidden: Bool {
        get { defaults.bool(forKey: "namesHidden") }
        set { defaults.set(newValue, forKey: "namesHidden") }
    }

    /// The gibberish seed: while names are hidden the gibberish stays the same and does not flicker.
    var scrambleSeed: Int {
        get { defaults.integer(forKey: "scrambleSeed") }
        set { defaults.set(newValue, forKey: "scrambleSeed") }
    }

    /// Open at login is turned on once, on the first run from /Applications. After that it is the user's call.
    var loginItemOffered: Bool {
        get { defaults.bool(forKey: "loginItemOffered") }
        set { defaults.set(newValue, forKey: "loginItemOffered") }
    }

    func arrange(_ profiles: [Profile]) -> [Profile] {
        let saved = order
        let byName = Dictionary(profiles.map { ($0.name, $0) }, uniquingKeysWith: { first, _ in first })
        return saved.compactMap { byName[$0] } + profiles.filter { !saved.contains($0.name) }
    }

    /// A new order of the tabs in the row. Hidden and collapsed profiles keep their places among the rest.
    func reorder(visible: [String], among profiles: [Profile]) {
        var next = visible.makeIterator()
        let shown = Set(visible)
        order = arrange(profiles).map { shown.contains($0.name) ? next.next() ?? $0.name : $0.name }
    }
}

enum LoginItem {
    static var isOn: Bool { SMAppService.mainApp.status == .enabled }

    static func set(_ on: Bool) {
        do {
            if on {
                try SMAppService.mainApp.register()
            } else {
                try SMAppService.mainApp.unregister()
            }
        } catch {
            NSLog("Google Chrome Profile Tabs: login item \(on ? "register" : "unregister") failed: \(error)")
        }
    }
}
