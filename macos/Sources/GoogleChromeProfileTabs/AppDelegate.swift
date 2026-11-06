import AppKit
import ApplicationServices

@MainActor final class AppDelegate: NSObject, NSApplicationDelegate {
    private var controller: Controller?
    private var trustPoll: Timer?

    func applicationDidFinishLaunching(_ notification: Notification) {
        guard let target = BrowserTarget.resolve() else {
            let alert = NSAlert()
            alert.messageText = "Google Chrome not found"
            alert.informativeText = "Google Chrome Profile Tabs shows Google Chrome profiles. Install Chrome and open Google Chrome Profile Tabs again."
            NSApp.activate()
            alert.runModal()
            NSApp.terminate(nil)
            return
        }
        if !target.isTest, let id = Bundle.main.bundleIdentifier,
           NSRunningApplication.runningApplications(withBundleIdentifier: id).contains(where: { $0 != .current }) {
            NSApp.terminate(nil)
            return
        }
        log.notice("launched for \(target.bundleID, privacy: .public), accessibility: \(AXIsProcessTrusted(), privacy: .public)")
        if AXIsProcessTrusted() {
            start(target)
            return
        }
        // Without Accessibility access the browser's windows can be neither seen nor moved. macOS shows a
        // prompt with a link to the settings, and the strip waits until access is turned on.
        AXIsProcessTrustedWithOptions(["AXTrustedCheckOptionPrompt": true] as CFDictionary)
        trustPoll = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] timer in
            MainActor.assumeIsolated {
                guard AXIsProcessTrusted() else { return }
                timer.invalidate()
                self?.start(target)
            }
        }
    }

    private func start(_ target: BrowserTarget) {
        log.notice("starting")
        BackgroundCursor.enable()
        let settings = Settings(isTest: target.isTest)
        if !target.isTest, !settings.loginItemOffered, Bundle.main.bundlePath.hasPrefix("/Applications/") {
            LoginItem.set(true)
            settings.loginItemOffered = true
        }
        let controller = Controller(target: target, settings: settings)
        controller.start()
        self.controller = controller
    }
}
