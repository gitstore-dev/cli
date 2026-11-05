import ContainerAPIClient
import ContainerResource
import Foundation
import Darwin
import Virtualization

@main
struct AppleProbe {
    static func main() async throws {
        let args = CommandLine.arguments
        let modes = ["--observe", "--exec-check", "--guest-recovery-check", "--guest-offline-check", "--guest-image-check"]
        guard args.count == 3, modes.contains(args[1]),
              args[2] == "gitstore-m2-linux-a1a13c", geteuid() != 0 else {
            throw NSError(domain: "GitStoreFixtureSelection", code: 2)
        }
        let client = ContainerClient()
        let snapshot = try await client.get(id: args[2])
        guard snapshot.id == args[2], snapshot.status == .running else {
            throw NSError(domain: "GitStoreFixtureIdentity", code: 3)
        }
        var guestIdentityVerified = false
        if args[1] != "--observe" {
            let output = Pipe()
            let errors = FileHandle(forWritingAtPath: "/dev/null")!
            defer { try? errors.close() }
            let configuration = ProcessConfiguration(
                executable: "/usr/bin/id", arguments: ["-u"], environment: ["PATH=/nonexistent"],
                workingDirectory: "/home/gitstorefixture", user: .id(uid: 11000, gid: 11000))
            let process = try await client.createProcess(
                containerId: args[2], processId: "gitstore-feasibility-" + UUID().uuidString.lowercased(),
                configuration: configuration, stdio: [nil, output.fileHandleForWriting, errors])
            try await process.start()
            try output.fileHandleForWriting.close()
            guard try await process.wait() == 0 else {
                throw NSError(domain: "GitStoreGuestProcess", code: 4)
            }
            var descriptor = pollfd(fd: output.fileHandleForReading.fileDescriptor, events: Int16(POLLIN), revents: 0)
            guard Darwin.poll(&descriptor, 1, 5000) == 1 else {
                throw NSError(domain: "GitStoreGuestOutput", code: 5)
            }
            var bytes = [UInt8](repeating: 0, count: 32)
            let count = Darwin.read(descriptor.fd, &bytes, bytes.count)
            guard count == 6, String(bytes: bytes.prefix(Int(count)), encoding: .utf8) == "11000\n" else {
                throw NSError(domain: "GitStoreGuestIdentity", code: 6)
            }
            try output.fileHandleForReading.close()
            guestIdentityVerified = true
        }
        var guestScenario = "none"
        var guestScenarioPassed = false
        if args[1].hasPrefix("--guest-") {
            let selected: (String, String)
            switch args[1] {
            case "--guest-recovery-check":
                selected = ("recovery", "test_killed_manager_reopens_owned_resources_for_public_stop")
            case "--guest-offline-check":
                selected = ("core-offline", "test_offline_layouts_authorize_only_the_selected_verified_bytes")
            default:
                selected = ("exp-003b-sdk", "test_actual_image_library_against_synthetic_private_registry")
            }
            guestScenario = selected.0
            let logURL = URL(fileURLWithPath: FileManager.default.currentDirectoryPath)
                .appendingPathComponent("build/apple-feasibility/guest-" + UUID().uuidString.lowercased() + ".log")
            let descriptor = Darwin.open(logURL.path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, mode_t(0o600))
            guard descriptor >= 3 else { throw NSError(domain: "GitStoreGuestEvidence", code: 7) }
            let log = FileHandle(fileDescriptor: descriptor, closeOnDealloc: true)
            let configuration = ProcessConfiguration(
                executable: "/usr/bin/python3",
                arguments: ["/home/gitstorefixture/experiments/source/tests/runtime/run.py", "--scenario", selected.0,
                            "--fixture", "/home/gitstorefixture/experiments/mac-library-fixture.json", "--case", selected.1],
                environment: ["PATH=/usr/bin:/bin", "LANG=C"],
                workingDirectory: "/home/gitstorefixture/experiments/source", user: .id(uid: 11000, gid: 11000))
            let process = try await client.createProcess(
                containerId: args[2], processId: "gitstore-feasibility-" + UUID().uuidString.lowercased(),
                configuration: configuration, stdio: [nil, log, log])
            try await process.start()
            guard try await process.wait() == 0 else {
                throw NSError(domain: "GitStoreGuestScenario", code: 8)
            }
            try log.close()
            // The guest process status and its bounded structured runner result
            // must both agree. Never interpret an accepted operation as ready.
            for _ in 0..<50 {
                let attributes = try FileManager.default.attributesOfItem(atPath: logURL.path)
                guard let size = attributes[.size] as? NSNumber, size.uint64Value <= 16 * 1024 * 1024 else {
                    throw NSError(domain: "GitStoreGuestEvidenceLimit", code: 9)
                }
                let data = try Data(contentsOf: logURL)
                for line in data.split(separator: 10) {
                    if let value = try? JSONSerialization.jsonObject(with: Data(line)) as? [String: Any],
                       value["scenario"] as? String == selected.0, value["status"] as? String == "passed" {
                        guestScenarioPassed = true
                    }
                }
                if guestScenarioPassed { break }
                try await Task.sleep(for: .milliseconds(100))
            }
            guard guestScenarioPassed else { throw NSError(domain: "GitStoreGuestReceipt", code: 10) }
        }
        let result: [String: Any] = [
            "schemaVersion": 1,
            "scope": args[1] == "--observe" ? "read-only ordinary Swift dependency probe" : "bounded guest process through ordinary Swift dependency",
            "fixture": args[2], "running": true, "invokingUid": geteuid(),
            "fixtureImageDigest": snapshot.configuration.image.descriptor.digest,
            "hostMountCount": snapshot.configuration.mounts.count,
            "sshForwarding": snapshot.configuration.ssh,
            "publishedPortCount": snapshot.configuration.publishedPorts.count,
            "publishedSocketCount": snapshot.configuration.publishedSockets.count,
            "virtualizationSupported": VZVirtualMachine.isSupported,
            "wrappedContainerExecutable": false, "publicBackend": false,
            "guestUid11000Verified": guestIdentityVerified,
            "guestScenario": guestScenario, "guestScenarioPassed": guestScenarioPassed
        ]
        let data = try JSONSerialization.data(withJSONObject: result, options: [.sortedKeys])
        FileHandle.standardOutput.write(data)
        FileHandle.standardOutput.write(Data([10]))
    }
}
