#![allow(dead_code)]
use std::{fs, os::unix::fs::PermissionsExt, path::PathBuf, process::Command};
use tempfile::TempDir;

pub struct DaemonFixture {
    pub root: TempDir,
    pub binary: PathBuf,
}
impl DaemonFixture {
    pub fn new() -> Self {
        let binary = std::env::var_os("GITSTORED_TEST_BINARY")
            .expect("Set GITSTORED_TEST_BINARY to an explicitly built test daemon");
        let binary = PathBuf::from(binary).canonicalize().unwrap();
        let root = tempfile::tempdir().unwrap();
        fs::set_permissions(root.path(), fs::Permissions::from_mode(0o700)).unwrap();
        for name in ["config", "state", "runtime", "cache", "home"] {
            let path = root.path().join(name);
            fs::create_dir(&path).unwrap();
            fs::set_permissions(path, fs::Permissions::from_mode(0o700)).unwrap();
        }
        Self { root, binary }
    }
    pub fn command(&self) -> Command {
        let mut c = Command::new(&self.binary);
        c.env_clear()
            .env("HOME", self.root.path().join("home"))
            .env("XDG_CONFIG_HOME", self.root.path().join("config"))
            .env("XDG_STATE_HOME", self.root.path().join("state"))
            .env("XDG_RUNTIME_DIR", self.root.path().join("runtime"))
            .env("XDG_CACHE_HOME", self.root.path().join("cache"))
            .env("PATH", "/nonexistent")
            .current_dir(self.root.path());
        c
    }
}
