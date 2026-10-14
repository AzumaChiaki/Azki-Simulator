// swift-tools-version: 6.2
import PackageDescription

let package = Package(
    name: "AzkiSimulator",
    platforms: [.macOS(.v15)],
    products: [.executable(name: "AzkiSimulator", targets: ["AzkiSimulator"])],
    targets: [
        .executableTarget(name: "AzkiSimulator", resources: [.copy("Runtime")])
    ],
    swiftLanguageModes: [.v5]
)
