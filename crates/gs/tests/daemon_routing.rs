use std::process::{Command, Output};
fn run(args: &[&str]) -> Output {
    let root = tempfile::tempdir().unwrap();
    let bad = root.path().join("remote.toml");
    std::fs::write(&bad, "invalid remote configuration {{{").unwrap();
    Command::new(env!("CARGO_BIN_EXE_gs"))
        .env_clear()
        .env("PATH", "/nonexistent")
        .env("HOME", root.path().canonicalize().unwrap())
        .env("GITSTORE_CONFIG", bad)
        .env(
            "GITSTORE_DAEMON_SOCKET",
            root.path().canonicalize().unwrap().join("absent.sock"),
        )
        .env("GITSTORE_CONTEXT", "unrelated-remote")
        .args(args)
        .output()
        .unwrap()
}
#[test]
fn local_routes_before_remote_context_or_credentials() {
    for args in [
        vec!["deployment", "list", "-o", "json"],
        vec!["api", "config", "validate", "-o", "json"],
        vec!["git", "config", "validate", "-o", "json"],
        vec!["serve", "--deployment", "local", "--detach", "-o", "json"],
    ] {
        let output = run(&args);
        assert_eq!(
            output.status.code(),
            Some(4),
            "{}",
            String::from_utf8_lossy(&output.stderr)
        );
        let error: serde_json::Value = serde_json::from_slice(&output.stderr).unwrap();
        assert_eq!(error["code"], "NOT_FOUND");
        assert!(output.stdout.is_empty());
    }
}
#[test]
fn local_preflight_rejects_bad_inputs_before_socket_lookup() {
    for args in [
        vec!["deployment", "list", "-o", "markdown"],
        vec!["serve", "--timeout", "2h"],
        vec!["serve", "--timeout", "1ns"],
        vec!["serve", "--add", "unregistered"],
        vec!["deployment", "logs", "dep1", "-o", "yaml"],
        vec!["serve", "--credential-stdin"],
        vec!["serve", "--detach"],
        vec!["deployment", "inspect", "dep1", "--wait", "ready"],
        vec!["deployment", "list", "--wait", "ready"],
    ] {
        let output = run(&args);
        assert_eq!(
            output.status.code(),
            Some(2),
            "{args:?}: {}",
            String::from_utf8_lossy(&output.stderr)
        );
    }
}
#[test]
fn remote_wait_gate_and_help_remain_independent() {
    let output = run(&["list", "products", "--wait", "ready", "-o", "json"]);
    assert_eq!(output.status.code(), Some(6));
    assert!(String::from_utf8_lossy(&output.stderr).contains("DEP-004"));
    for args in [
        vec!["--help"],
        vec!["--version"],
        vec!["api", "graphql", "--help"],
        vec!["deployment", "--help"],
    ] {
        assert!(run(&args).status.success(), "{args:?}");
    }
    let catalog = run(&["api-resources", "-o", "json"]);
    assert!(catalog.status.success());
    assert!(String::from_utf8_lossy(&catalog.stdout).contains("gitstore-alpha9-2026-11-05"));
}
