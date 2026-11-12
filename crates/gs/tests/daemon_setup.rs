#[path = "support/daemon.rs"]
mod daemon;

#[test]
fn explicit_missing_socket_never_starts_a_manager_or_loads_remote_credentials() {
    let root = tempfile::tempdir().unwrap();
    let socket = root.path().join("missing.sock");
    let result = std::process::Command::new(env!("CARGO_BIN_EXE_gs"))
        .env_clear()
        .env("PATH", "/nonexistent")
        .env("HOME", root.path())
        .env("XDG_CONFIG_HOME", root.path())
        .env("XDG_STATE_HOME", root.path())
        .env("XDG_RUNTIME_DIR", root.path())
        .arg("--daemon-socket")
        .arg(&socket)
        .args(["deployment", "list", "-o", "json"])
        .output()
        .unwrap();
    assert!(!result.status.success());
    assert!(result.stdout.is_empty());
    assert!(!socket.exists());
    assert_eq!(std::fs::read_dir(root.path()).unwrap().count(), 0);
    let error: serde_json::Value = serde_json::from_slice(&result.stderr).unwrap();
    assert_ne!(error["stage"], "auth");
}

#[test]
fn automatic_connection_refuses_a_running_mismatched_application_without_mutation() {
    use gs::daemon_protocol::{read_frame, write_frame};
    use serde_json::{Value, json};
    use std::os::unix::{fs::PermissionsExt, net::UnixListener};
    use std::time::Duration;

    let root = tempfile::tempdir().unwrap();
    std::fs::set_permissions(root.path(), std::fs::Permissions::from_mode(0o700)).unwrap();
    let path = root.path().canonicalize().unwrap().join("manager.sock");
    let listener = UnixListener::bind(&path).unwrap();
    std::fs::set_permissions(&path, std::fs::Permissions::from_mode(0o600)).unwrap();
    let examples: Vec<Value> = serde_json::from_slice(include_bytes!(
        "../../../tests/fixtures/daemon/examples.json"
    ))
    .unwrap();
    let limits = examples
        .iter()
        .find(|item| item["example"] == "EX-001" && item["kind"] == "response")
        .unwrap()["message"]["result"]["limits"]
        .clone();
    let server = std::thread::spawn(move || {
        let (mut stream, _) = listener.accept().unwrap();
        stream
            .set_read_timeout(Some(Duration::from_secs(3)))
            .unwrap();
        let request = read_frame(&mut stream).unwrap().unwrap();
        assert_eq!(request["method"], "system.negotiate");
        write_frame(&mut stream, &json!({"version":{"major":1,"minor":0},
            "requestId":request["requestId"],"result":{
            "selectedVersion":{"major":1,"minor":0},"capabilities":["system.read"],"limits":limits}})).unwrap();
        let request = read_frame(&mut stream).unwrap().unwrap();
        assert_eq!(request["method"], "system.capabilities");
        write_frame(
            &mut stream,
            &json!({"version":{"major":1,"minor":0},
            "requestId":request["requestId"],"result":{"implementationVersion":"0.0.0-mismatch"}}),
        )
        .unwrap();
        assert!(read_frame(&mut stream).unwrap().is_none());
    });
    let error = gs::daemon_start::connect(&path, &["system.read"], Duration::from_secs(2), true)
        .err()
        .unwrap();
    assert_eq!(error.code, "INCOMPATIBLE_VERSION");
    assert_eq!(error.exit, 6);
    server.join().unwrap();
    assert!(
        path.exists(),
        "must not remove or replace the old manager socket"
    );
}

#[cfg(target_os = "macos")]
#[test]
fn automatic_macos_runtime_is_explicitly_unsupported_without_user_state() {
    let root = tempfile::tempdir().unwrap();
    let canonical = root.path().canonicalize().unwrap();
    let result = std::process::Command::new(env!("CARGO_BIN_EXE_gs"))
        .env_clear()
        .env("HOME", &canonical)
        .env("PATH", "/nonexistent")
        .env("XDG_RUNTIME_DIR", &canonical)
        .env("XDG_STATE_HOME", &canonical)
        .args(["deployment", "list", "-o", "json"])
        .output()
        .unwrap();
    assert_eq!(
        result.status.code(),
        Some(6),
        "{}",
        String::from_utf8_lossy(&result.stderr)
    );
    let error: serde_json::Value = serde_json::from_slice(&result.stderr).unwrap();
    assert_eq!(error["code"], "UNSUPPORTED_HOST");
    assert!(result.stdout.is_empty());
    assert_eq!(std::fs::read_dir(root.path()).unwrap().count(), 0);
}

#[cfg(target_os = "macos")]
#[test]
fn macos_without_xdg_runtime_reports_unsupported_host_without_creating_state() {
    for args in [
        vec!["deployment", "list", "-o", "json"],
        vec!["serve", "--deployment", "local", "--detach", "-o", "json"],
    ] {
        let root = tempfile::tempdir().unwrap();
        let canonical = root.path().canonicalize().unwrap();
        let result = std::process::Command::new(env!("CARGO_BIN_EXE_gs"))
            .env_clear()
            .env("HOME", &canonical)
            .env("PATH", "/nonexistent")
            .args(&args)
            .output()
            .unwrap();
        assert_eq!(
            result.status.code(),
            Some(6),
            "{args:?}: {}",
            String::from_utf8_lossy(&result.stderr)
        );
        assert!(result.stdout.is_empty());
        let error: serde_json::Value = serde_json::from_slice(&result.stderr).unwrap();
        assert_eq!(error["code"], "UNSUPPORTED_HOST");
        assert!(error["message"].as_str().unwrap().contains("Linux"));
        assert_eq!(std::fs::read_dir(root.path()).unwrap().count(), 0);
    }
}

#[cfg(target_os = "macos")]
#[test]
fn macos_explicit_socket_without_xdg_runtime_remains_connect_only() {
    let root = tempfile::tempdir().unwrap();
    let canonical = root.path().canonicalize().unwrap();
    let result = std::process::Command::new(env!("CARGO_BIN_EXE_gs"))
        .env_clear()
        .env("HOME", &canonical)
        .env("PATH", "/nonexistent")
        .arg("--daemon-socket")
        .arg(canonical.join("missing.sock"))
        .args(["deployment", "list", "-o", "json"])
        .output()
        .unwrap();
    assert_eq!(result.status.code(), Some(4));
    let error: serde_json::Value = serde_json::from_slice(&result.stderr).unwrap();
    assert_eq!(error["code"], "NOT_FOUND");
    assert!(result.stdout.is_empty());
    assert_eq!(std::fs::read_dir(root.path()).unwrap().count(), 0);
}

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
