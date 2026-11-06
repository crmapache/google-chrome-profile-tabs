// Draws the app icon, a browser window with a row of colored profile tabs above it, and writes it for
// both platforms: macos/Resources/AppIcon.icns and windows/res/app.ico.
// Run from the repository root: swift icon/make-icon.swift
import AppKit

func color(_ hex: UInt32) -> NSColor {
    NSColor(
        srgbRed: CGFloat((hex >> 16) & 0xFF) / 255,
        green: CGFloat((hex >> 8) & 0xFF) / 255,
        blue: CGFloat(hex & 0xFF) / 255,
        alpha: 1
    )
}

func render(_ pixels: Int) -> Data {
    let rep = NSBitmapImageRep(
        bitmapDataPlanes: nil, pixelsWide: pixels, pixelsHigh: pixels, bitsPerSample: 8, samplesPerPixel: 4,
        hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0
    )!
    rep.size = NSSize(width: 1024, height: 1024)
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)

    // The plate on the macOS icon grid: an 824 square with 185 corners in the middle of a 1024 canvas.
    let plate = NSRect(x: 100, y: 100, width: 824, height: 824)
    let platePath = NSBezierPath(roundedRect: plate, xRadius: 185, yRadius: 185)
    NSGradient(starting: color(0x2A2C31), ending: color(0x141416))!.draw(in: platePath, angle: -90)

    // The tab row: the selected tab is lighter, each has a circle in its profile color.
    let bar = NSRect(x: 196, y: 640, width: 632, height: 112)
    let barPath = NSBezierPath(roundedRect: bar, xRadius: 26, yRadius: 26)
    color(0x0E0E10).setFill()
    barPath.fill()
    let tabColors: [UInt32] = [0x8FD694, 0xFADF73, 0xF3B2BE]
    let tabWidth = bar.width / CGFloat(tabColors.count)
    NSGraphicsContext.saveGraphicsState()
    barPath.addClip()
    color(0x4A4B51).setFill()
    NSRect(x: bar.minX + tabWidth, y: bar.minY, width: tabWidth, height: bar.height).fill()
    color(0xFADF73).setFill()
    NSRect(x: bar.minX + tabWidth, y: bar.minY, width: tabWidth, height: 12).fill()
    NSGraphicsContext.restoreGraphicsState()
    for (index, hex) in tabColors.enumerated() {
        let center = NSPoint(x: bar.minX + tabWidth * (CGFloat(index) + 0.5), y: bar.midY + 4)
        color(hex).setFill()
        NSBezierPath(ovalIn: NSRect(x: center.x - 26, y: center.y - 26, width: 52, height: 52)).fill()
    }

    // The browser window under the row.
    let window = NSRect(x: 196, y: 228, width: 632, height: 388)
    let windowPath = NSBezierPath(roundedRect: window, xRadius: 34, yRadius: 34)
    color(0x3A3C42).setFill()
    windowPath.fill()
    NSGraphicsContext.saveGraphicsState()
    windowPath.addClip()
    color(0x55575E).setFill()
    NSRect(x: window.minX, y: window.maxY - 74, width: window.width, height: 74).fill()
    NSGraphicsContext.restoreGraphicsState()
    color(0x6B6E76).setFill()
    for (index, width) in [420.0, 520.0, 360.0].enumerated() {
        let line = NSRect(x: window.minX + 52, y: window.maxY - 150 - CGFloat(index) * 64, width: width, height: 26)
        NSBezierPath(roundedRect: line, xRadius: 13, yRadius: 13).fill()
    }

    NSGraphicsContext.restoreGraphicsState()
    return rep.representation(using: .png, properties: [:])!
}

// macOS: an iconset turned into .icns by iconutil.
let iconset = URL(fileURLWithPath: "build/AppIcon.iconset")
try? FileManager.default.removeItem(at: iconset)
try! FileManager.default.createDirectory(at: iconset, withIntermediateDirectories: true)
for size in [16, 32, 128, 256, 512] {
    try! render(size).write(to: iconset.appending(path: "icon_\(size)x\(size).png"))
    try! render(size * 2).write(to: iconset.appending(path: "icon_\(size)x\(size)@2x.png"))
}
let iconutil = Process()
iconutil.executableURL = URL(fileURLWithPath: "/usr/bin/iconutil")
iconutil.arguments = ["-c", "icns", iconset.path, "-o", "macos/Resources/AppIcon.icns"]
try! iconutil.run()
iconutil.waitUntilExit()

// Windows: an .ico holding PNG images, from the taskbar size to the large one.
let sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
let images = sizes.map(render)
var ico = Data([0, 0, 1, 0, UInt8(sizes.count), 0])
var offset = 6 + 16 * sizes.count
for (size, image) in zip(sizes, images) {
    let side = UInt8(size >= 256 ? 0 : size)
    ico.append(contentsOf: [side, side, 0, 0, 1, 0, 32, 0])
    withUnsafeBytes(of: UInt32(image.count).littleEndian) { ico.append(contentsOf: $0) }
    withUnsafeBytes(of: UInt32(offset).littleEndian) { ico.append(contentsOf: $0) }
    offset += image.count
}
images.forEach { ico.append($0) }
try! ico.write(to: URL(fileURLWithPath: "windows/res/app.ico"))
print("Wrote macos/Resources/AppIcon.icns and windows/res/app.ico")
