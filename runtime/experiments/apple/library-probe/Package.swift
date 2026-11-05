// swift-tools-version: 6.2
import PackageDescription

let package = Package(
    name: "GitStoreAppleFeasibility",
    platforms: [.macOS("26.0")],
    dependencies: [
        .package(url: "https://github.com/apple/container.git",
                 revision: "aa8d76b44a834eee3ebf6a0bc577e376f65f6361"),
        .package(url: "https://github.com/apple/containerization.git", exact: "0.53.0")
    ],
    targets: [
        .executableTarget(name: "AppleProbe", dependencies: [
            .product(name: "ContainerAPIClient", package: "container"),
            .product(name: "ContainerResource", package: "container")
        ]),
        .executableTarget(name: "DirectGuard", dependencies: [
            .product(name: "Containerization", package: "containerization")
        ])
    ]
)
