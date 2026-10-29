mod support;
use support::*;
#[test]
fn help_and_version_are_local() {
    let h = Harness::new();
    let o = h.run(&["--help"]);
    assert!(o.status.success());
    assert!(stdout(&o).contains("GitStore"));
    let o = h.run(&["--version"]);
    assert!(stdout(&o).contains("0.1.0"));
}
#[test]
fn selection_is_atomic_and_reports_sources() {
    let h = Harness::new();
    h.configure("https://example.test/team/graphql");
    let o = h.run(&["context", "show", "-o", "json"]);
    assert!(o.status.success(), "{}", stderr(&o));
    let v: serde_json::Value = serde_json::from_slice(&o.stdout).unwrap();
    assert_eq!(v["account"], "alice");
    assert_eq!(v["sources"]["account"], "context");
    let original = std::fs::read(h.config()).unwrap();
    let o = h.run(&["context", "use", "missing"]);
    assert_eq!(o.status.code(), Some(2));
    assert_eq!(std::fs::read(h.config()).unwrap(), original);
    assert!(h.run(&["auth", "switch", "bob"]).status.success());
    assert!(stdout(&h.run(&["context", "show", "-o", "json"])).contains("bob"));
}
#[test]
fn explicit_token_output_and_safe_status() {
    let h = Harness::new();
    h.configure("https://example.test/graphql");
    let p = h.write("token", "synthetic-only\n");
    let o = h.run(&[
        "--credential-file",
        p.to_str().unwrap(),
        "auth",
        "status",
        "-o",
        "json",
    ]);
    assert!(o.status.success(), "{}", stderr(&o));
    assert!(!stdout(&o).contains("synthetic-only"));
    let o = h.run(&["auth", "token", "--credential-file", p.to_str().unwrap()]);
    assert_eq!(stdout(&o), "synthetic-only\n");
    let o = h.run(&[
        "auth",
        "token",
        "--credential-file",
        p.to_str().unwrap(),
        "-o",
        "json",
    ]);
    assert_eq!(o.status.code(), Some(2));
    assert!(!stderr(&o).contains("synthetic-only"));
}
#[test]
fn protected_file_and_endpoint_isolation() {
    let h = Harness::new();
    h.configure("https://example.test/a/graphql");
    let p = h.write("token", "secret");
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        std::fs::set_permissions(&p, std::fs::Permissions::from_mode(0o644)).unwrap();
        let o = h.run(&["auth", "token", "--credential-file", p.to_str().unwrap()]);
        assert_eq!(o.status.code(), Some(3));
    }
    let o = h.run(&[
        "--endpoint",
        "https://example.test/b/graphql",
        "context",
        "show",
    ]);
    assert_eq!(o.status.code(), Some(2));
}

#[test]
fn flag_environment_precedence_and_invalid_config() {
    let h = Harness::new();
    h.configure("https://example.test/graphql");
    let o = h
        .cmd()
        .env("GITSTORE_NAMESPACE", "environment")
        .args(["context", "show", "--namespace", "explicit", "-o", "json"])
        .output()
        .unwrap();
    assert!(o.status.success());
    let v: serde_json::Value = serde_json::from_slice(&o.stdout).unwrap();
    assert_eq!(v["namespace"], "explicit");
    assert_eq!(v["sources"]["namespace"], "flag");
    let o = h
        .cmd()
        .env("GITSTORE_NAMESPACE", "environment")
        .args(["context", "show", "-o", "json"])
        .output()
        .unwrap();
    let v: serde_json::Value = serde_json::from_slice(&o.stdout).unwrap();
    assert_eq!(v["namespace"], "environment");
    assert_eq!(v["sources"]["namespace"], "environment");
    let original = std::fs::read_to_string(h.config()).unwrap();
    std::fs::write(h.config(), original.replace("version = 1", "version = 2")).unwrap();
    assert_eq!(h.run(&["config", "validate"]).status.code(), Some(2));
}
#[test]
fn concurrent_selection_updates_preserve_valid_configuration() {
    let h = Harness::new();
    h.configure("https://example.test/graphql");
    let mut children = vec![];
    for n in ["alice", "bob", "alice", "bob"] {
        children.push(
            h.cmd()
                .args(["auth", "switch", n])
                .stdout(std::process::Stdio::null())
                .spawn()
                .unwrap(),
        );
    }
    for mut child in children {
        assert!(child.wait().unwrap().success())
    }
    assert!(h.run(&["config", "validate"]).status.success());
}
#[test]
#[cfg(unix)]
fn failed_atomic_write_preserves_original() {
    use std::os::unix::fs::PermissionsExt;
    if unsafe { libc::geteuid() } == 0 {
        return;
    }
    let h = Harness::new();
    h.configure("https://example.test/graphql");
    let original = std::fs::read(h.config()).unwrap();
    h.write("config.toml.lock", "");
    std::fs::set_permissions(h.dir.path(), std::fs::Permissions::from_mode(0o500)).unwrap();
    let o = h.run(&["auth", "switch", "bob"]);
    std::fs::set_permissions(h.dir.path(), std::fs::Permissions::from_mode(0o700)).unwrap();
    assert_eq!(o.status.code(), Some(2));
    assert_eq!(std::fs::read(h.config()).unwrap(), original);
}
#[test]
fn argument_errors_in_machine_mode_are_safe_json() {
    let h = Harness::new();
    let o = h.run(&["--output", "json", "--secret-mistake", "do-not-print-this"]);
    assert_eq!(o.status.code(), Some(2));
    let v: serde_json::Value = serde_json::from_slice(&o.stderr).unwrap();
    assert_eq!(v["code"], "INVALID_INPUT");
    assert!(!stderr(&o).contains("do-not-print-this"));
}
