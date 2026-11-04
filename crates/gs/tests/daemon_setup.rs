#[path = "support/daemon.rs"]
mod daemon;

#[test]
#[ignore = "requires explicitly built C daemon; run scripts/test-daemon.sh"]
fn isolated_daemon_setup_and_gated_execution() {
    let f = daemon::DaemonFixture::new();
    let result = f.command().arg("--version").output().unwrap();
    assert!(result.status.success());
    assert!(
        String::from_utf8(result.stdout)
            .unwrap()
            .contains("protocol 1.0")
    );
    let result = f.command().arg("--arbitrary-exec").output().unwrap();
    assert_eq!(result.status.code(), Some(2));
    assert!(result.stdout.is_empty());
    assert_eq!(
        std::fs::read_dir(f.root.path().join("state"))
            .unwrap()
            .count(),
        0
    );
}

#[test]
#[ignore = "requires explicitly built C daemon; run scripts/test-daemon.sh"]
fn native_negotiation_preserves_unavailable_runtime_gates() {
    use gs::daemon_client::Client;
    use serde_json::json;
    use std::{
        process::{Child, Command},
        time::{Duration, Instant},
    };
    struct Guard(Child);
    impl Drop for Guard {
        fn drop(&mut self) {
            let _ = self.0.kill();
            let _ = self.0.wait();
        }
    }
    let fixture = daemon::DaemonFixture::new();
    let runtime = fixture.root.path().join("runtime").canonicalize().unwrap();
    let socket = runtime.join("gitstored.sock");
    let mut child = Guard(
        fixture
            .command()
            .arg("--runtime-dir")
            .arg(&runtime)
            .spawn()
            .unwrap(),
    );
    let until = Instant::now() + Duration::from_secs(5);
    while !socket.exists() {
        assert!(Instant::now() < until, "daemon did not bind");
        assert!(child.0.try_wait().unwrap().is_none(), "daemon exited");
        std::thread::sleep(Duration::from_millis(5));
    }
    let timeout = Duration::from_secs(2);
    let mut client = Client::connect(&socket, &["system.read"], timeout).unwrap();
    let caps = client
        .call("system.capabilities", json!({}), None, timeout)
        .unwrap();
    assert_eq!(
        caps["capabilities"],
        json!(["system.read", "deployment.validate"])
    );
    let error = client
        .call("deployment.start", json!({}), Some("safe-key"), timeout)
        .unwrap_err();
    assert_eq!(error.code, "UNSUPPORTED_CAPABILITY");
    let output = Command::new(env!("CARGO_BIN_EXE_gs"))
        .env_clear()
        .env("PATH", "/nonexistent")
        .env("GITSTORE_CONFIG", runtime.join("invalid-remote.toml"))
        .args([
            "--daemon-socket",
            socket.to_str().unwrap(),
            "deployment",
            "list",
            "-o",
            "json",
        ])
        .output()
        .unwrap();
    assert_eq!(
        output.status.code(),
        Some(6),
        "{}",
        String::from_utf8_lossy(&output.stderr)
    );
    let error: serde_json::Value = serde_json::from_slice(&output.stderr).unwrap();
    assert_eq!(error["code"], "UNSUPPORTED_CAPABILITY");
    unsafe {
        libc::kill(child.0.id() as i32, libc::SIGTERM);
    }
    let until = Instant::now() + Duration::from_secs(5);
    while child.0.try_wait().unwrap().is_none() {
        assert!(Instant::now() < until, "daemon did not stop");
        std::thread::sleep(Duration::from_millis(5));
    }
    assert!(!socket.exists());
    assert!(
        std::fs::read_dir(fixture.root.path().join("state"))
            .unwrap()
            .next()
            .is_none()
    );
}
