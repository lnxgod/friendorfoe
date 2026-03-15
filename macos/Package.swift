// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "FriendOrFoe",
    platforms: [
        .macOS(.v14)
    ],
    targets: [
        .executableTarget(
            name: "FriendOrFoe",
            path: "Sources/FriendOrFoe",
            linkerSettings: [
                .linkedFramework("CoreWLAN"),
                .linkedFramework("CoreBluetooth"),
                .linkedFramework("CoreLocation"),
                .linkedFramework("MapKit"),
            ]
        )
    ]
)
