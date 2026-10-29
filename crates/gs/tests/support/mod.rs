#![allow(dead_code)]
use std::{
    fs,
    path::Path,
    process::{Command, Output},
};
use tempfile::TempDir;
pub struct Harness {
    pub dir: TempDir,
}
impl Harness {
    pub fn new() -> Self {
        Self {
            dir: tempfile::tempdir().unwrap(),
        }
    }
    pub fn config(&self) -> std::path::PathBuf {
        self.dir.path().join("config.toml")
    }
    pub fn write(&self, name: &str, data: &str) -> std::path::PathBuf {
        let path = self.dir.path().join(name);
        fs::write(&path, data).unwrap();
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            fs::set_permissions(&path, fs::Permissions::from_mode(0o600)).unwrap();
        }
        path
    }
    pub fn configure(&self, endpoint: &str) {
        self.write(
            "config.toml",
            &format!(
                r#"version = 1
current_context = "one"
[contexts.one]
endpoint = "{endpoint}"
allow_loopback_http = true
contract_profile = "m0-2026-10-27"
account = "alice"
namespace = "shop"
[contexts.one.accounts.alice]
issuer = "test"
[contexts.one.accounts.bob]
issuer = "test"
"#
            ),
        );
    }
    pub fn cmd(&self) -> Command {
        let mut c = Command::new(env!("CARGO_BIN_EXE_gs"));
        c.env_clear()
            .env("HOME", self.dir.path())
            .env("XDG_CONFIG_HOME", self.dir.path())
            .env("PATH", "/nonexistent")
            .env("GIT_CONFIG_NOSYSTEM", "1")
            .arg("--config")
            .arg(self.config())
            .current_dir(self.dir.path());
        c
    }
    pub fn run(&self, args: &[&str]) -> Output {
        self.cmd().args(args).output().unwrap()
    }
}
pub fn stdout(o: &Output) -> String {
    String::from_utf8(o.stdout.clone()).unwrap()
}
pub fn stderr(o: &Output) -> String {
    String::from_utf8(o.stderr.clone()).unwrap()
}
pub fn fixture(path: &str) -> std::path::PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("tests/fixtures")
        .join(path)
}
