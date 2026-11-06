// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "GoogleChromeProfileTabs",
    platforms: [.macOS(.v14)],
    targets: [
        .executableTarget(
            name: "GoogleChromeProfileTabs",
            path: "Sources/GoogleChromeProfileTabs",
            linkerSettings: [
                .linkedFramework("AppKit"),
                .linkedFramework("Carbon"),
                .linkedFramework("ServiceManagement"),
            ]
        ),
    ],
    swiftLanguageModes: [.v5]
)
