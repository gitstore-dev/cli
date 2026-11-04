#[path = "support/daemon.rs"]
mod daemon;
use std::{
    process::{Child, Command, Output},
    time::{Duration, Instant},
};
struct Manager {
    fixture: daemon::DaemonFixture,
    child: Child,
}
impl Manager {
    fn new() -> Self {
        let fixture = daemon::DaemonFixture::new();
        let seed = fixture.binary.with_file_name("state_fixture");
        let state = fixture.root.path().canonicalize().unwrap().join("state");
        assert!(
            Command::new(seed)
                .env_clear()
                .args(["seed", state.join("state.db").to_str().unwrap()])
                .status()
                .unwrap()
                .success()
        );
        let mut result = Self {
            child: fixture
                .command()
                .args([
                    "--runtime-dir",
                    fixture
                        .root
                        .path()
                        .canonicalize()
                        .unwrap()
                        .join("runtime")
                        .to_str()
                        .unwrap(),
                    "--state-dir",
                    state.to_str().unwrap(),
                ])
                .spawn()
                .unwrap(),
            fixture,
        };
        let deadline = Instant::now() + Duration::from_secs(5);
        while !result.socket().exists() {
            assert!(Instant::now() < deadline);
            assert!(result.child.try_wait().unwrap().is_none());
            std::thread::sleep(Duration::from_millis(5));
        }
        result
    }
    fn restart(&mut self) {
        unsafe {
            libc::kill(self.child.id() as i32, libc::SIGTERM);
        }
        let deadline = Instant::now() + Duration::from_secs(5);
        while self.child.try_wait().unwrap().is_none() {
            assert!(Instant::now() < deadline);
            std::thread::sleep(Duration::from_millis(5));
        }
        self.child = self
            .fixture
            .command()
            .args([
                "--runtime-dir",
                self.fixture
                    .root
                    .path()
                    .canonicalize()
                    .unwrap()
                    .join("runtime")
                    .to_str()
                    .unwrap(),
                "--state-dir",
                self.fixture
                    .root
                    .path()
                    .canonicalize()
                    .unwrap()
                    .join("state")
                    .to_str()
                    .unwrap(),
            ])
            .spawn()
            .unwrap();
        let deadline = Instant::now() + Duration::from_secs(5);
        while !self.socket().exists() {
            assert!(Instant::now() < deadline);
            assert!(self.child.try_wait().unwrap().is_none());
            std::thread::sleep(Duration::from_millis(5));
        }
    }
    fn socket(&self) -> std::path::PathBuf {
        self.fixture
            .root
            .path()
            .canonicalize()
            .unwrap()
            .join("runtime/gitstored.sock")
    }
    fn run(&self, args: &[&str]) -> Output {
        Command::new(env!("CARGO_BIN_EXE_gs"))
            .env_clear()
            .env("PATH", "/nonexistent")
            .env("GITSTORE_DAEMON_SOCKET", self.socket())
            .env("GITSTORE_CONFIG", "/missing/remote.toml")
            .env("GITSTORE_CONTEXT", "unrelated-remote")
            .args(args)
            .output()
            .unwrap()
    }
}
impl Drop for Manager {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}
#[test]
#[ignore = "requires explicitly built daemon; run scripts/test-daemon.sh"]
fn seeded_records_formats_paging_and_identity_preserving_deadline() {
    let mut manager = Manager::new();
    for format in ["table", "json", "yaml"] {
        let output = manager.run(&["deployment", "list", "--limit", "1", "-o", format]);
        assert!(
            output.status.success(),
            "{}",
            String::from_utf8_lossy(&output.stderr)
        );
        assert!(!output.stdout.is_empty());
        let inspection = manager.run(&["deployment", "inspect", "d1", "-o", format]);
        assert!(inspection.status.success());
        assert!(!inspection.stdout.is_empty());
        if format == "json" {
            let page: serde_json::Value = serde_json::from_slice(&output.stdout).unwrap();
            let output = manager.run(&[
                "deployment",
                "list",
                "--cursor",
                page["nextCursor"].as_str().unwrap(),
                "-o",
                "json",
            ]);
            let page: serde_json::Value = serde_json::from_slice(&output.stdout).unwrap();
            assert_eq!(page["deployments"][0]["deploymentId"], "d2");
        }
    }
    manager.restart();
    let state = manager
        .fixture
        .root
        .path()
        .canonicalize()
        .unwrap()
        .join("state");
    let other = manager
        .fixture
        .root
        .path()
        .canonicalize()
        .unwrap()
        .join("runtime-other");
    let output = manager
        .fixture
        .command()
        .args([
            "--runtime-dir",
            other.to_str().unwrap(),
            "--state-dir",
            state.to_str().unwrap(),
        ])
        .output()
        .unwrap();
    assert_eq!(output.status.code(), Some(5));
    let output = manager.run(&[
        "deployment",
        "inspect",
        "d2",
        "--operation",
        "op1",
        "-o",
        "json",
    ]);
    assert_eq!(output.status.code(), Some(5));
    let output = manager.run(&[
        "deployment",
        "inspect",
        "d1",
        "--operation",
        "op1",
        "--wait",
        "ready",
        "--timeout",
        "30ms",
        "-o",
        "json",
    ]);
    assert_eq!(
        output.status.code(),
        Some(7),
        "{}",
        String::from_utf8_lossy(&output.stderr)
    );
    let error: serde_json::Value = serde_json::from_slice(&output.stderr).unwrap();
    assert_eq!(error["operationId"], "op1");
    assert_eq!(error["deploymentId"], "d1");
    let output = manager.run(&[
        "deployment",
        "inspect",
        "d1",
        "--operation",
        "op1",
        "-o",
        "json",
    ]);
    assert!(output.status.success());
    let record: serde_json::Value = serde_json::from_slice(&output.stdout).unwrap();
    assert_eq!(record["operation"]["state"], "running");
    assert_eq!(record["deployment"]["observedGeneration"], 1);
    let output = manager.run(&["deployment", "inspect", "missing", "-o", "json"]);
    assert_eq!(output.status.code(), Some(4));
    let output = manager.run(&["deployment", "list", "--cursor", "foreign", "-o", "json"]);
    assert_eq!(output.status.code(), Some(8));
}
