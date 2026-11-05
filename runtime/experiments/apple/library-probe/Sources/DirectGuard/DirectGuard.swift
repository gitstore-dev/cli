import Containerization
import Foundation

// No guest, kernel, filesystem or network is ever created by this guard test.
struct NeverBoot: VirtualMachineManager {
    func create(config: some VMCreationConfig) async throws -> any VirtualMachineInstance {
        throw NSError(domain: "UnexpectedGuestBoot", code: 1)
    }
}

@main
struct DirectGuard {
    static func main() throws {
        var configuration = LinuxContainer.Configuration()
        configuration.seccompProfile = .default
        do {
            _ = try LinuxContainer(
                "gitstore-direct-policy-guard",
                rootfs: .block(format: "ext4", source: "/nonexistent", destination: "/"),
                vmm: NeverBoot(), configuration: configuration)
            throw NSError(domain: "UnenforcedGuestPolicy", code: 2)
        } catch {
            guard String(describing: error).contains("seccompProfile requires ociRuntimePath") else {
                throw error
            }
        }
        print("{\"directWithoutGuestRuntimeRejected\":true,\"guestBooted\":false,\"publicBackend\":false}")
    }
}
