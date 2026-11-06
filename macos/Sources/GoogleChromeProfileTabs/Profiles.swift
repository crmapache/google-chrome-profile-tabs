import AppKit

struct Profile {
    let name: String
    /// Names the profile may go by at the end of a window title.
    let titleNames: [String]
    let color: NSColor
    let picture: NSImage?
    /// A fingerprint to compare lists: did anything visible change.
    let signature: String
}

/// The browser's profiles. The list and the names come from Chrome's own Profiles menu: it is reachable
/// through Accessibility, while macOS does not let apps read Chrome's files without Full Disk Access. If
/// "Local State" is readable after all, it adds Chrome's order, the profile colors and the account photo.
@MainActor final class ProfileStore {
    private struct Details {
        let titleNames: [String]
        let color: NSColor
        let picture: NSImage?
        let pictureStamp: String
        let position: Int
    }

    private let target: BrowserTarget
    private(set) var profiles: [Profile] = []
    private var details: [String: Details] = [:]
    private var stamp: Date?
    private var reportedFailure = false

    init(target: BrowserTarget) {
        self.target = target
    }

    private var localState: URL { target.userDataDir.appending(path: "Local State") }

    /// Updates the list from the menu's names. Returns true if the profile list changed.
    @discardableResult func update(names: [String]) -> Bool {
        loadDetailsIfChanged()
        let ordered = names.enumerated().sorted { left, right in
            let a = details[left.element]?.position ?? Int.max
            let b = details[right.element]?.position ?? Int.max
            return a == b ? left.offset < right.offset : a < b
        }
        let fresh = ordered.map { profile(named: $0.element) }
        let changed = fresh.map(\.signature) != profiles.map(\.signature)
        profiles = fresh
        return changed
    }

    private func profile(named name: String) -> Profile {
        let extra = details[name]
        let color = extra?.color ?? Self.fallbackColor(for: name)
        return Profile(
            name: name,
            titleNames: extra?.titleNames ?? [name],
            color: color,
            picture: extra?.picture,
            signature: "\(name)|\(color.description)|\(extra?.pictureStamp ?? "")"
        )
    }

    // MARK: Local State

    private func loadDetailsIfChanged() {
        let mtime = (try? FileManager.default.attributesOfItem(atPath: localState.path))?[.modificationDate] as? Date
        guard mtime == nil || mtime != stamp else { return }
        let data: Data
        do {
            data = try Data(contentsOf: localState)
        } catch {
            if !reportedFailure {
                reportedFailure = true
                log.notice("profile colors unavailable, \(self.localState.path, privacy: .public) is not readable: \(error.localizedDescription, privacy: .public)")
            }
            return
        }
        // A half-written file is skipped: the previous data stays until the next write.
        guard let root = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let section = root["profile"] as? [String: Any],
              let cache = section["info_cache"] as? [String: [String: Any]] else { return }
        stamp = mtime
        let order = section["profiles_order"] as? [String] ?? []
        var fresh: [String: Details] = [:]
        for (dir, entry) in cache {
            let (name, more) = details(dir: dir, entry: entry, position: order.firstIndex(of: dir) ?? order.count)
            fresh[name] = more
        }
        details = fresh
    }

    private func details(dir: String, entry: [String: Any], position: Int) -> (String, Details) {
        let local = entry["name"] as? String ?? dir
        let given = entry["gaia_given_name"] as? String ?? ""
        let full = entry["gaia_name"] as? String ?? ""
        // A profile without a name of its own is called after its Google account: "Your Chrome" in the file, the given name in the menu.
        let name = entry["is_using_default_name"] as? Bool == true
            ? [given, full, local].first { !$0.isEmpty } ?? local
            : local
        var titleNames: [String] = []
        for candidate in [name, local, given, full] where !candidate.isEmpty && !titleNames.contains(candidate) {
            titleNames.append(candidate)
        }

        let seed = (entry["profile_color_seed"] ?? entry["default_avatar_fill_color"]) as? NSNumber
        var picture: NSImage?
        var pictureStamp = ""
        if let file = entry["gaia_picture_file_name"] as? String, !file.isEmpty {
            let url = target.userDataDir.appending(path: dir).appending(path: file)
            picture = NSImage(contentsOf: url)
            let mtime = (try? FileManager.default.attributesOfItem(atPath: url.path))?[.modificationDate] as? Date
            pictureStamp = "\(mtime?.timeIntervalSince1970 ?? 0)"
        }
        return (name, Details(
            titleNames: titleNames,
            color: seed.map { Self.color(argb: UInt32(truncatingIfNeeded: $0.int64Value)) } ?? Self.fallbackColor(for: name),
            picture: picture,
            pictureStamp: pictureStamp,
            position: position
        ))
    }

    // MARK: Colors

    /// Chrome stores colors as signed ARGB numbers.
    private static func color(argb value: UInt32) -> NSColor {
        NSColor(
            srgbRed: CGFloat((value >> 16) & 0xFF) / 255,
            green: CGFloat((value >> 8) & 0xFF) / 255,
            blue: CGFloat(value & 0xFF) / 255,
            alpha: 1
        )
    }

    /// A profile color when Chrome's are out of reach: fixed per name, from a calm palette.
    private static func fallbackColor(for name: String) -> NSColor {
        let palette: [UInt32] = [0x8AB4F8, 0x81C995, 0xFDD663, 0xF28B82, 0xC58AF9, 0x78D9EC, 0xFCAD70, 0xFF8BCB]
        let hash = name.unicodeScalars.reduce(UInt32(5381)) { ($0 &* 33) &+ $1.value }
        return color(argb: palette[Int(hash % UInt32(palette.count))])
    }
}
