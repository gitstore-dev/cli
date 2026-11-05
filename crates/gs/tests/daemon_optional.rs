use std::process::Command;

#[test]
fn optional_prerequisites_fail_before_manager_discovery() {
    let root = tempfile::tempdir().unwrap();
    for (component, prerequisite) in [
        ("oidc", "API issuer integration"),
        ("admin", "Admin UI"),
        ("storefront", "publication reads"),
    ] {
        for args in [vec![component, "serve"], vec!["serve", "--add", component]] {
            let output = Command::new(env!("CARGO_BIN_EXE_gs"))
                .env_clear()
                .env("HOME", root.path())
                .env("PATH", "/nonexistent")
                .args([
                    "--daemon-socket",
                    "/missing/optional-manager.sock",
                    "-o",
                    "json",
                ])
                .args(args)
                .args(["--allow-provision", "--detach", "--non-interactive"])
                .output()
                .unwrap();
            assert_eq!(output.status.code(), Some(6));
            assert!(String::from_utf8_lossy(&output.stderr).contains(prerequisite));
            assert!(output.stdout.is_empty());
            assert_eq!(std::fs::read_dir(root.path()).unwrap().count(), 0);
        }
    }
}

#[test]
fn duplicate_optional_selection_is_usage_error() {
    let output = Command::new(env!("CARGO_BIN_EXE_gs"))
        .env_clear()
        .args(["serve", "--add", "admin", "--add", "admin"])
        .output()
        .unwrap();
    assert_eq!(output.status.code(), Some(2));
    assert!(String::from_utf8_lossy(&output.stderr).contains("must be unique"));
}
