import AppKit

/// The browser whose profiles the strip shows. Google Chrome by default; the environment variables
/// GOOGLE_CHROME_PROFILE_TABS_BROWSER (path to the .app) and GOOGLE_CHROME_PROFILE_TABS_USER_DATA (its data
/// folder) point it at a separate copy, such as Chrome for Testing, to test without touching the real Chrome.
struct BrowserTarget {
    let bundleID: String
    let appURL: URL
    let userDataDir: URL
    /// The browser's name in window titles: "Page - Google Chrome - Work".
    let titleName: String
    let isTest: Bool

    static func resolve() -> BrowserTarget? {
        let env = ProcessInfo.processInfo.environment
        if let path = env["GOOGLE_CHROME_PROFILE_TABS_BROWSER"], let data = env["GOOGLE_CHROME_PROFILE_TABS_USER_DATA"] {
            let url = URL(fileURLWithPath: path)
            guard let bundle = Bundle(url: url), let id = bundle.bundleIdentifier else { return nil }
            return BrowserTarget(
                bundleID: id,
                appURL: url,
                userDataDir: URL(fileURLWithPath: data),
                titleName: displayName(of: bundle),
                isTest: true
            )
        }
        let id = "com.google.Chrome"
        guard let url = NSWorkspace.shared.urlForApplication(withBundleIdentifier: id),
              let bundle = Bundle(url: url) else { return nil }
        return BrowserTarget(
            bundleID: id,
            appURL: url,
            userDataDir: FileManager.default.homeDirectoryForCurrentUser
                .appending(path: "Library/Application Support/Google/Chrome"),
            titleName: displayName(of: bundle),
            isTest: false
        )
    }

    private static func displayName(of bundle: Bundle) -> String {
        bundle.object(forInfoDictionaryKey: "CFBundleDisplayName") as? String
            ?? bundle.object(forInfoDictionaryKey: "CFBundleName") as? String
            ?? "Google Chrome"
    }
}
