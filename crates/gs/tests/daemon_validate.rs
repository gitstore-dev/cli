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
    fn start() -> Self {
        let fixture = daemon::DaemonFixture::new();
        let runtime = fixture.root.path().join("runtime").canonicalize().unwrap();
        let child = fixture
            .command()
            .arg("--runtime-dir")
            .arg(runtime)
            .spawn()
            .unwrap();
        let mut result = Self { fixture, child };
        let until = Instant::now() + Duration::from_secs(5);
        while !result.socket().exists() {
            assert!(Instant::now() < until);
            assert!(result.child.try_wait().unwrap().is_none());
            std::thread::sleep(Duration::from_millis(5));
        }
        result
    }
    fn socket(&self) -> std::path::PathBuf {
        self.fixture
            .root
            .path()
            .canonicalize()
            .unwrap()
            .join("runtime/gitstored.sock")
    }
    fn command(&self, args: &[&str]) -> Command {
        let mut command = Command::new(env!("CARGO_BIN_EXE_gs"));
        command
            .env_clear()
            .env("PATH", "/nonexistent")
            .env(
                "HOME",
                self.fixture
                    .root
                    .path()
                    .canonicalize()
                    .unwrap()
                    .join("home"),
            )
            .env(
                "XDG_CONFIG_HOME",
                self.fixture
                    .root
                    .path()
                    .canonicalize()
                    .unwrap()
                    .join("config"),
            )
            .env("GITSTORE_DAEMON_SOCKET", self.socket())
            .env("GITSTORE_CONFIG", "/missing/unrelated-remote.toml")
            .env("GITSTORE_PROFILE", "unknown-from-env")
            .env("GITSTORE_PROFILE_VERSION", "1")
            .args(args);
        command
    }
    fn run(&self, args: &[&str]) -> Output {
        self.command(args).output().unwrap()
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
fn readonly_native_preflight_and_local_selection() {
    let manager = Manager::start();
    for format in ["json", "yaml", "table"] {
        let output = manager.run(&[
            "api",
            "config",
            "validate",
            "--profile",
            "core",
            "--non-interactive",
            "-o",
            format,
        ]);
        assert_eq!(
            output.status.code(),
            Some(8),
            "{}",
            String::from_utf8_lossy(&output.stderr)
        );
        assert!(!output.stdout.is_empty());
        if format == "json" {
            let plan: serde_json::Value = serde_json::from_slice(&output.stdout).unwrap();
            assert_eq!(plan["valid"], false);
            assert_eq!(plan["effectivePlan"]["profileId"], "core");
            let diagnostic = plan["diagnostics"].to_string();
            assert!(diagnostic.contains("NATIVE_VALIDATION_UNPERFORMED"));
            assert!(diagnostic.contains("BACKEND_UNAVAILABLE"));
        }
    }
    let output = manager.run(&["git", "config", "validate", "-o", "json"]);
    let plan: serde_json::Value = serde_json::from_slice(&output.stdout).unwrap();
    assert_eq!(plan["diagnostics"][0]["code"], "PROFILE_NOT_FOUND");
    for directory in ["config", "state", "cache"] {
        assert_eq!(
            std::fs::read_dir(manager.fixture.root.path().join(directory))
                .unwrap()
                .count(),
            0
        );
    }
}

#[test]
#[ignore = "requires explicitly built daemon; run scripts/test-daemon.sh"]
fn local_config_precedence_and_secret_safe_errors() {
    use std::{fs, os::unix::fs::PermissionsExt};
    let manager = Manager::start();
    let directory = manager.fixture.root.path().join("config/gitstore");
    fs::create_dir(&directory).unwrap();
    fs::set_permissions(&directory, fs::Permissions::from_mode(0o700)).unwrap();
    let file = directory.join("local.toml");
    let text = "version = 1\n[selection]\nprofile = 'core'\nprofile_version = '1'\ndatastore = 'scylladb'\n";
    fs::write(&file, text).unwrap();
    fs::set_permissions(&file, fs::Permissions::from_mode(0o600)).unwrap();
    let output = manager
        .command(&["controller", "config", "validate", "-o", "json"])
        .env_remove("GITSTORE_PROFILE")
        .env_remove("GITSTORE_PROFILE_VERSION")
        .output()
        .unwrap();
    assert_eq!(
        output.status.code(),
        Some(8),
        "{}",
        String::from_utf8_lossy(&output.stderr)
    );
    let plan: serde_json::Value = serde_json::from_slice(&output.stdout).unwrap();
    assert_eq!(plan["effectivePlan"]["profileId"], "core");
    assert_eq!(plan["effectivePlan"]["datastore"], "scylladb");
    assert_eq!(
        plan["effectivePlan"]["configuration"][0]["datastoreBackend"],
        "scylla"
    );
    assert!(
        !plan["diagnostics"]
            .to_string()
            .contains("DATASTORE_PROFILE_UNAVAILABLE")
    );
    assert_eq!(plan["requiredAssets"].as_array().unwrap().len(), 4);
    assert_eq!(fs::read_to_string(&file).unwrap(), text);
    fs::write(
        &file,
        "version = 1\npassword = 'synthetic-secret-do-not-print'\n",
    )
    .unwrap();
    let output = manager.run(&[
        "api",
        "config",
        "validate",
        "--profile",
        "core",
        "-o",
        "json",
    ]);
    assert_eq!(output.status.code(), Some(2));
    assert!(!String::from_utf8_lossy(&output.stderr).contains("synthetic-secret"));
    assert!(output.stdout.is_empty());
}
