import CoreGraphics

@_silgen_name("_CGSDefaultConnection")
private func _CGSDefaultConnection() -> Int32

@_silgen_name("CGSSetConnectionProperty")
private func CGSSetConnectionProperty(_ connection: Int32, _ target: Int32, _ key: CFString, _ value: CFTypeRef) -> Int32

/// The cursor over the strip. The strip belongs to a background app (Chrome is in front), and the window
/// server changes the cursor only for the app in front. This connection property lifts that; there is no
/// public way, and every utility that draws over other apps' windows uses it.
enum BackgroundCursor {
    static func enable() {
        let connection = _CGSDefaultConnection()
        let result = CGSSetConnectionProperty(connection, connection, "SetsCursorInBackground" as CFString, kCFBooleanTrue)
        if result != 0 {
            log.error("background cursor not allowed: \(result, privacy: .public)")
        }
    }
}
