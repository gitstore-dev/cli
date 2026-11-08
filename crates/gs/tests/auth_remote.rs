mod support;
use serde_json::{Value, json};
use std::{path::Path, process::Output};
use support::{Harness, stderr};
use tiny_http::{Response, Server};

fn configure(h: &Harness, endpoint: &str) {
    h.configure(endpoint);
    let text = std::fs::read_to_string(h.config())
        .unwrap()
        .replace("m0-2026-10-27", "gitstore-alpha9-2026-11-05");
    h.write("config.toml", &(text+"\n[contexts.one.accounts.alice.auth]\nversion=1\nprovider=\"static-users\"\nusername=\"alice\"\n"));
}
fn invoke(h: &Harness, session: &Path, arguments: &[&str]) -> Output {
    h.cmd()
        .args([
            "--credential-file",
            session.to_str().unwrap(),
            "-o",
            "json",
            "auth",
        ])
        .args(arguments)
        .output()
        .unwrap()
}
fn no_secrets(out: &Output) {
    let all =
        String::from_utf8_lossy(&out.stdout).to_string() + &String::from_utf8_lossy(&out.stderr);
    for secret in [
        "synthetic-password",
        "synthetic-session-1",
        "synthetic-session-2",
    ] {
        assert!(!all.contains(secret), "Secret leaked from auth command");
    }
}
fn issued(field: &str, token: &str) -> Value {
    json!({"data":{field:{"token":{"accessToken":token,"refreshToken":token,"tokenType":"Bearer","expiresIn":3600,"scope":null,"idToken":null}}}})
}

#[test]
fn login_rotation_replay_and_remote_logout_preserve_scoped_persistence() {
    let h = Harness::new();
    let server = Server::http("127.0.0.1:0").unwrap();
    configure(
        &h,
        &format!("http://{}/proxy/admin/graphql", server.server_addr()),
    );
    let password = h.write("password", "synthetic-password\n");
    let session = h.dir.path().join("session");
    let replay = h.write("old-session", "synthetic-session-1\n");
    let worker = std::thread::spawn(move || {
        for step in 0..4 {
            let mut request = server
                .recv_timeout(std::time::Duration::from_secs(5))
                .unwrap()
                .unwrap();
            assert_eq!(request.url(), "/proxy/admin/graphql");
            let authorization = request
                .headers()
                .iter()
                .find(|header| header.field.equiv("authorization"))
                .map(|header| header.value.as_str().to_owned());
            let mut bytes = String::new();
            request.as_reader().read_to_string(&mut bytes).unwrap();
            let body: Value = serde_json::from_str(&bytes).unwrap();
            let reply = match step {
                0 => {
                    assert!(authorization.is_none());
                    assert_eq!(
                        body["variables"]["input"],
                        json!({"username":"alice","password":"synthetic-password"})
                    );
                    issued("login", "synthetic-session-1")
                }
                1 => {
                    assert!(
                        authorization.is_none(),
                        "Refresh must not send a possibly expired bearer"
                    );
                    assert_eq!(
                        body["variables"]["input"]["refreshToken"],
                        "synthetic-session-1"
                    );
                    issued("refreshToken", "synthetic-session-2")
                }
                2 => {
                    assert_eq!(
                        body["variables"]["input"]["refreshToken"],
                        "synthetic-session-1"
                    );
                    json!({"data":null,"errors":[{"message":"revoked synthetic-session-1"}]})
                }
                _ => {
                    assert_eq!(authorization.as_deref(), Some("Bearer synthetic-session-2"));
                    assert!(body["query"].as_str().unwrap().contains("logout{success}"));
                    json!({"data":{"logout":{"success":true}}})
                }
            };
            request
                .respond(Response::from_string(reply.to_string()))
                .unwrap();
        }
        assert!(
            server
                .recv_timeout(std::time::Duration::from_millis(100))
                .unwrap()
                .is_none(),
            "Auth mutations must never be retried blindly"
        );
    });
    let login = invoke(
        &h,
        &session,
        &[
            "login",
            "--username",
            "alice",
            "--password-file",
            password.to_str().unwrap(),
        ],
    );
    assert!(login.status.success(), "{}", stderr(&login));
    no_secrets(&login);
    assert_eq!(
        std::fs::read_to_string(&session).unwrap(),
        "synthetic-session-1\n"
    );
    use std::os::unix::fs::MetadataExt;
    assert_eq!(session.metadata().unwrap().mode() & 0o777, 0o600);
    assert_eq!(session.metadata().unwrap().nlink(), 1);
    let refresh = invoke(&h, &session, &["refresh"]);
    assert!(refresh.status.success(), "{}", stderr(&refresh));
    no_secrets(&refresh);
    assert_eq!(
        std::fs::read_to_string(&session).unwrap(),
        "synthetic-session-2\n"
    );
    let raw = h.run(&[
        "--credential-file",
        session.to_str().unwrap(),
        "auth",
        "token",
    ]);
    assert_eq!(
        String::from_utf8(raw.stdout).unwrap(),
        "synthetic-session-2\n"
    );
    let failed = invoke(&h, &replay, &["refresh"]);
    assert_eq!(failed.status.code(), Some(3));
    no_secrets(&failed);
    assert_eq!(
        std::fs::read_to_string(&replay).unwrap(),
        "synthetic-session-1\n"
    );
    let logout = invoke(&h, &session, &["logout", "--remote"]);
    assert!(logout.status.success(), "{}", stderr(&logout));
    no_secrets(&logout);
    assert!(!session.exists());
    worker.join().unwrap();
}

#[test]
fn consumed_refresh_with_concurrent_local_change_reports_uncertainty_without_overwriting() {
    let h = Harness::new();
    let server = Server::http("127.0.0.1:0").unwrap();
    configure(
        &h,
        &format!("http://{}/admin/graphql", server.server_addr()),
    );
    let session = h.write("session", "synthetic-session-1\n");
    let changed = session.clone();
    let worker = std::thread::spawn(move || {
        let request = server
            .recv_timeout(std::time::Duration::from_secs(5))
            .unwrap()
            .unwrap();
        std::fs::write(changed, "concurrent-session\n").unwrap();
        request
            .respond(Response::from_string(
                issued("refreshToken", "synthetic-session-2").to_string(),
            ))
            .unwrap();
        assert!(
            server
                .recv_timeout(std::time::Duration::from_millis(150))
                .unwrap()
                .is_none()
        );
    });
    let out = invoke(&h, &session, &["refresh"]);
    worker.join().unwrap();
    assert_eq!(out.status.code(), Some(8));
    no_secrets(&out);
    let error: Value = serde_json::from_slice(&out.stderr).unwrap();
    assert_eq!(error["code"], "AUTH_PERSISTENCE_UNCERTAIN");
    assert_eq!(error["submission"], "unknown");
    assert_eq!(
        std::fs::read_to_string(session).unwrap(),
        "concurrent-session\n"
    );
}

#[test]
fn real_persistence_io_failure_after_issued_rotation_is_uncertain_and_not_retried() {
    use std::os::unix::fs::PermissionsExt;
    assert_ne!(
        unsafe { libc::geteuid() },
        0,
        "This test requires the ordinary fixture user"
    );
    let h = Harness::new();
    let server = Server::http("127.0.0.1:0").unwrap();
    configure(
        &h,
        &format!("http://{}/admin/graphql", server.server_addr()),
    );
    let session = h.write("session", "synthetic-session-1\n");
    let parent = h.dir.path().to_owned();
    struct Restore(std::path::PathBuf);
    impl Drop for Restore {
        fn drop(&mut self) {
            let _ = std::fs::set_permissions(&self.0, std::fs::Permissions::from_mode(0o700));
        }
    }
    let restore = Restore(parent.clone());
    let worker = std::thread::spawn(move || {
        let request = server
            .recv_timeout(std::time::Duration::from_secs(5))
            .unwrap()
            .unwrap();
        std::fs::set_permissions(&parent, std::fs::Permissions::from_mode(0o500)).unwrap();
        request
            .respond(Response::from_string(
                issued("refreshToken", "synthetic-session-2").to_string(),
            ))
            .unwrap();
        assert!(
            server
                .recv_timeout(std::time::Duration::from_millis(150))
                .unwrap()
                .is_none()
        );
    });
    let out = invoke(&h, &session, &["refresh"]);
    worker.join().unwrap();
    drop(restore);
    assert_eq!(out.status.code(), Some(8));
    no_secrets(&out);
    assert_eq!(
        serde_json::from_slice::<Value>(&out.stderr).unwrap()["code"],
        "AUTH_PERSISTENCE_UNCERTAIN"
    );
    assert_eq!(
        std::fs::read_to_string(session).unwrap(),
        "synthetic-session-1\n"
    );
}

#[test]
fn provider_username_and_stdin_conflicts_fail_before_credentials_or_network() {
    let h = Harness::new();
    configure(&h, "http://127.0.0.1:1/admin/graphql");
    let missing = h.dir.path().join("missing-parent/session");
    let out = invoke(
        &h,
        &missing,
        &[
            "login",
            "--username",
            "other",
            "--password-file",
            "/missing-password",
        ],
    );
    assert_eq!(out.status.code(), Some(2));
    let cfg = std::fs::read_to_string(h.config())
        .unwrap()
        .replace("provider=\"static-users\"", "provider=\"oidc\"");
    h.write("config.toml", &cfg);
    assert_eq!(invoke(&h, &missing, &["refresh"]).status.code(), Some(6));
    assert!(!missing.parent().unwrap().exists());
    let out = h.run(&[
        "--credential-stdin",
        "--non-interactive",
        "auth",
        "login",
        "--username",
        "alice",
        "--password-stdin",
    ]);
    assert_eq!(out.status.code(), Some(2));
}

#[test]
fn http_success_with_bad_payload_never_persists_or_removes_credentials() {
    for reply in [
        json!({"data":{"refreshToken":{"token":{"accessToken":"synthetic-session-2","refreshToken":"different-session","tokenType":"Bearer","expiresIn":3600}}}}),
        json!({"data":{"refreshToken":{"token":null}}}),
        json!({"data":null,"errors":[{"message":"reflected synthetic-session-1"}]}),
    ] {
        let h = Harness::new();
        let server = Server::http("127.0.0.1:0").unwrap();
        configure(
            &h,
            &format!("http://{}/admin/graphql", server.server_addr()),
        );
        let session = h.write("session", "synthetic-session-1\n");
        let worker = std::thread::spawn(move || {
            let request = server
                .recv_timeout(std::time::Duration::from_secs(5))
                .unwrap()
                .unwrap();
            request
                .respond(Response::from_string(reply.to_string()))
                .unwrap();
            assert!(
                server
                    .recv_timeout(std::time::Duration::from_millis(100))
                    .unwrap()
                    .is_none()
            );
        });
        let out = invoke(&h, &session, &["refresh"]);
        worker.join().unwrap();
        assert!(!out.status.success());
        no_secrets(&out);
        assert_eq!(
            std::fs::read_to_string(session).unwrap(),
            "synthetic-session-1\n"
        );
    }
    let h = Harness::new();
    let server = Server::http("127.0.0.1:0").unwrap();
    configure(
        &h,
        &format!("http://{}/admin/graphql", server.server_addr()),
    );
    let session = h.write("session", "synthetic-session-1\n");
    let worker = std::thread::spawn(move || {
        server
            .recv_timeout(std::time::Duration::from_secs(5))
            .unwrap()
            .unwrap()
            .respond(Response::from_string(
                json!({"data":{"logout":{"success":false}}}).to_string(),
            ))
            .unwrap();
    });
    let out = invoke(&h, &session, &["logout", "--remote"]);
    worker.join().unwrap();
    assert_eq!(out.status.code(), Some(3));
    no_secrets(&out);
    assert!(session.exists());
}

#[test]
fn redirected_password_stdin_and_secret_identity_are_explicit() {
    use clap::Parser;
    use std::io::Write;
    use std::process::Stdio;
    let h = Harness::new();
    let server = Server::http("127.0.0.1:0").unwrap();
    configure(
        &h,
        &format!("http://{}/admin/graphql", server.server_addr()),
    );
    let session = h.dir.path().join("session");
    let worker = std::thread::spawn(move || {
        let mut request = server
            .recv_timeout(std::time::Duration::from_secs(5))
            .unwrap()
            .unwrap();
        let mut body = String::new();
        request.as_reader().read_to_string(&mut body).unwrap();
        assert_eq!(
            serde_json::from_str::<Value>(&body).unwrap()["variables"]["input"]["password"],
            "synthetic-password"
        );
        request
            .respond(Response::from_string(
                issued("login", "synthetic-session-1").to_string(),
            ))
            .unwrap();
    });
    let mut child = h
        .cmd()
        .args([
            "--credential-file",
            session.to_str().unwrap(),
            "--non-interactive",
            "auth",
            "login",
            "--username",
            "alice",
            "--password-stdin",
        ])
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .unwrap();
    let mut stdin = child.stdin.take().unwrap();
    stdin.write_all(b"synthetic-password\n").unwrap();
    drop(stdin);
    let out = child.wait_with_output().unwrap();
    worker.join().unwrap();
    assert!(out.status.success(), "{}", stderr(&out));
    no_secrets(&out);
    let config = gs::config::load(&h.config()).unwrap();
    let effective = config
        .effective(&gs::cli::Cli::parse_from(["gs", "context", "show"]))
        .unwrap();
    let original = gs::credentials::key(&effective, "graphql");
    let runtime = gs::credentials::key(&effective, "runtime");
    let mut other = effective.clone();
    other
        .context
        .accounts
        .get_mut("alice")
        .unwrap()
        .auth
        .as_mut()
        .unwrap()
        .username = "another-user".into();
    assert_ne!(original, gs::credentials::key(&other, "graphql"));
    assert_eq!(runtime, gs::credentials::key(&other, "runtime"));
    other.context.endpoint = "http://127.0.0.1:2/admin/graphql".into();
    assert_ne!(original, gs::credentials::key(&other, "graphql"));
}
