mod support;
#[path = "support/watch.rs"]
mod wire;
use serde_json::{Value, json};
use std::{
    path::Path,
    process::{Output, Stdio},
    time::{Duration, Instant},
};
use support::{Harness, stderr};

fn configured(endpoint: &str) -> Harness {
    let h = Harness::new();
    h.configure(endpoint);
    h.write(
        "config.toml",
        &std::fs::read_to_string(h.config())
            .unwrap()
            .replace("m0-2026-10-27", "gitstore-alpha9-2026-11-05"),
    );
    h
}
fn invoke(h: &Harness, checkpoint: &Path, args: &[&str]) -> Output {
    h.credential_cmd("synthetic-watch-token")
        .args(["--timeout", "5s", "watch"])
        .args(args)
        .args(["--checkpoint-file", checkpoint.to_str().unwrap()])
        .output()
        .unwrap()
}
fn lines(out: &Output) -> Vec<Value> {
    assert!(!String::from_utf8_lossy(&out.stdout).contains("synthetic-watch-token"));
    assert!(!String::from_utf8_lossy(&out.stderr).contains("synthetic-watch-token"));
    String::from_utf8_lossy(&out.stdout)
        .lines()
        .map(|line| serde_json::from_str(line).unwrap())
        .collect()
}

#[test]
fn per_kind_bootstrap_and_typed_objects_use_journal_positions() {
    let evidence: Value =
        serde_json::from_str(include_str!("fixtures/upstream/alpha9-watch.json")).unwrap();
    for kind in ["namespace", "repository", "product", "category", "file"] {
        let (listener, endpoint) = wire::listener();
        let h = configured(&endpoint);
        let checkpoint = h.dir.path().join("watch.json");
        let expected = evidence["kinds"][kind].clone();
        let kind_owned = kind.to_owned();
        let worker = std::thread::spawn(move || {
            let mut socket = wire::accept(&listener);
            let subscription = wire::start(&mut socket);
            assert_eq!(
                subscription["payload"]["variables"]["resourceVersion"],
                expected["bootstrap"]
            );
            assert!(
                subscription["payload"]["query"]
                    .as_str()
                    .unwrap()
                    .contains(expected["field"].as_str().unwrap())
            );
            wire::send(
                &mut socket,
                wire::event(&kind_owned, "BOOKMARK", "journal-start"),
            );
            wire::send(
                &mut socket,
                wire::event(&kind_owned, "ADDED", "journal-after-1"),
            );
            assert_eq!(wire::read(&mut socket)["type"], "complete");
        });
        let out = invoke(&h, &checkpoint, &[kind, "--max-events", "1"]);
        assert!(out.status.success(), "{}", stderr(&out));
        worker.join().unwrap();
        let events = lines(&out);
        assert_eq!(events.len(), 2);
        assert_eq!(events[1]["resourceVersion"], "journal-after-1");
        assert_eq!(
            events[1]["object"]["metadata"]["resourceVersion"],
            "object-17"
        );
        let record: Value =
            serde_json::from_str(&std::fs::read_to_string(&checkpoint).unwrap()).unwrap();
        assert_eq!(record["resourceVersion"], "journal-after-1");
        assert_eq!(record["connectionState"], "stopped");
        assert!(
            !std::fs::read_to_string(checkpoint)
                .unwrap()
                .contains("synthetic-watch-token")
        );
    }
}

#[test]
fn reconnect_resumes_after_emitted_cursor_and_allows_duplicate_delivery() {
    let (listener, endpoint) = wire::listener();
    let h = configured(&endpoint);
    let checkpoint = h.dir.path().join("watch.json");
    let worker = std::thread::spawn(move || {
        let mut first = wire::accept(&listener);
        wire::start(&mut first);
        wire::send(
            &mut first,
            wire::event("product", "BOOKMARK", "journal-start"),
        );
        wire::send(&mut first, wire::event("product", "ADDED", "journal-1"));
        drop(first);
        let mut second = wire::accept(&listener);
        let resumed = wire::start(&mut second);
        assert_eq!(
            resumed["payload"]["variables"]["resourceVersion"],
            "journal-1"
        );
        wire::send(&mut second, wire::event("product", "ADDED", "journal-1"));
        wire::send(&mut second, wire::event("product", "MODIFIED", "journal-2"));
        assert_eq!(wire::read(&mut second)["type"], "complete");
    });
    let out = invoke(&h, &checkpoint, &["product", "--max-events", "3"]);
    assert!(out.status.success(), "{}", stderr(&out));
    worker.join().unwrap();
    let events = lines(&out);
    assert_eq!(events.len(), 4);
    assert_eq!(events[1]["object"], events[2]["object"]);
    assert_eq!(events[1]["resourceVersion"], events[2]["resourceVersion"]);
    assert_eq!(events[3]["resourceVersion"], "journal-2");
}

#[test]
fn idle_subscription_keeps_its_connection_cursor_and_retry_budget() {
    let (listener, endpoint) = wire::listener();
    let h = configured(&endpoint);
    let checkpoint = h.dir.path().join("idle-watch.json");
    let worker = std::thread::spawn(move || {
        let mut socket = wire::accept(&listener);
        wire::start(&mut socket);
        wire::send(
            &mut socket,
            wire::event("product", "BOOKMARK", "journal-start"),
        );
        // Exceed the real production read poll without events or heartbeats.
        std::thread::sleep(Duration::from_secs(16));
        wire::send(
            &mut socket,
            wire::event("product", "ADDED", "journal-after-idle"),
        );
        assert_eq!(wire::read(&mut socket)["type"], "complete");
        assert!(
            matches!(listener.accept(), Err(error) if error.kind() == std::io::ErrorKind::WouldBlock)
        );
    });
    let out = h
        .credential_cmd("synthetic-watch-token")
        .args([
            "--timeout",
            "30s",
            "watch",
            "product",
            "--max-events",
            "1",
            "--checkpoint-file",
        ])
        .arg(&checkpoint)
        .output()
        .unwrap();
    worker.join().unwrap();
    assert!(out.status.success(), "{}", stderr(&out));
    assert_eq!(
        lines(&out).last().unwrap()["resourceVersion"],
        "journal-after-idle"
    );
    let record: Value = serde_json::from_slice(&std::fs::read(checkpoint).unwrap()).unwrap();
    assert_eq!(record["retries"], 0);
    assert_eq!(record["connectionState"], "stopped");
}

#[test]
fn expiry_wrong_kind_and_oversized_frames_fail_without_advancing_past_valid_output() {
    for fault in [
        "expired",
        "kind",
        "namespace",
        "labels",
        "message-id",
        "frame",
    ] {
        let (listener, endpoint) = wire::listener();
        let h = configured(&endpoint);
        let checkpoint = h.dir.path().join("watch.json");
        let worker = std::thread::spawn(move || {
            let mut socket = wire::accept(&listener);
            wire::start(&mut socket);
            wire::send(
                &mut socket,
                wire::event("product", "BOOKMARK", "journal-start"),
            );
            let mut event = wire::event("product", "ADDED", "journal-bad");
            if fault == "kind" {
                event["payload"]["data"]["watchProducts"]["product"]["kind"] = json!("File");
            }
            if fault == "frame" {
                event["payload"]["data"]["watchProducts"]["product"]["body"] =
                    json!("x".repeat(4096));
            }
            if fault == "expired" {
                event = json!({"type":"error","id":"gs-watch","payload":[{"message":"must not log synthetic-watch-token","extensions":{"code":"WATCH_EXPIRED"}}]});
            }
            if fault == "namespace" {
                event["payload"]["data"]["watchProducts"]["namespace"] = json!("another");
                event["payload"]["data"]["watchProducts"]["product"]["metadata"]["namespace"] =
                    json!("another");
            }
            if fault == "labels" {
                event["payload"]["data"]["watchProducts"]["product"]["metadata"]["labels"]["app"] =
                    json!("other");
            }
            if fault == "message-id" {
                event["id"] = json!("another-subscription");
            }
            wire::send(&mut socket, event);
        });
        let out = invoke(
            &h,
            &checkpoint,
            &[
                "product",
                "--max-frame-bytes",
                "1024",
                "--selector",
                r#"{"matchLabels":{"app":"sample"}}"#,
            ],
        );
        worker.join().unwrap_or_else(|_| {
            panic!(
                "Fixture did not receive the expected connection: {}",
                stderr(&out)
            )
        });
        assert!(!out.status.success());
        let emitted = lines(&out);
        assert_eq!(emitted.len(), 1);
        let error: Value = serde_json::from_slice(&out.stderr).unwrap();
        assert_eq!(
            error["code"],
            match fault {
                "expired" => "WATCH_EXPIRED",
                "kind" | "namespace" | "labels" => "WATCH_SCOPE_MISMATCH",
                "message-id" => "WATCH_PROTOCOL",
                _ => "WATCH_FRAME_LIMIT",
            }
        );
        let record: Value =
            serde_json::from_str(&std::fs::read_to_string(checkpoint).unwrap()).unwrap();
        assert_eq!(record["resourceVersion"], "journal-start");
    }
}

#[test]
fn checkpoints_cannot_cross_installations_namespaces_kinds_or_selectors() {
    let (listener, endpoint) = wire::listener();
    let h = configured(&endpoint);
    let checkpoint = h.dir.path().join("watch.json");
    let worker = std::thread::spawn(move || {
        let mut socket = wire::accept(&listener);
        wire::start(&mut socket);
        wire::send(
            &mut socket,
            wire::event("product", "BOOKMARK", "journal-start"),
        );
        wire::send(&mut socket, wire::event("product", "ADDED", "journal-1"));
        assert_eq!(wire::read(&mut socket)["type"], "complete");
    });
    let first = invoke(&h, &checkpoint, &["product", "--max-events", "1"]);
    assert!(first.status.success(), "{}", stderr(&first));
    worker.join().unwrap();
    let before = std::fs::read(&checkpoint).unwrap();
    for arguments in [
        vec!["file", "--resource-version", "journal-1"],
        vec![
            "product",
            "--namespace",
            "another",
            "--resource-version",
            "journal-1",
        ],
        vec![
            "product",
            "--selector",
            r#"{"matchLabels":{"app":"other"}}"#,
            "--resource-version",
            "journal-1",
        ],
        vec!["product", "--resource-version", "object-17"],
        vec![
            "product",
            "different-name",
            "--resource-version",
            "journal-1",
        ],
    ] {
        let out = invoke(&h, &checkpoint, &arguments);
        assert_eq!(out.status.code(), Some(2), "{}", stderr(&out));
        assert_eq!(std::fs::read(&checkpoint).unwrap(), before);
    }
    let old = std::fs::read_to_string(h.config()).unwrap();
    h.write(
        "config.toml",
        &old.replace(&endpoint, "http://127.0.0.1:1/proxy/admin/graphql"),
    );
    assert_eq!(
        invoke(
            &h,
            &checkpoint,
            &["product", "--resource-version", "journal-1"]
        )
        .status
        .code(),
        Some(2)
    );
    assert_eq!(std::fs::read(&checkpoint).unwrap(), before);
}

#[test]
fn slow_observer_is_bounded_and_does_not_checkpoint_partial_output() {
    let (listener, endpoint) = wire::listener();
    let h = configured(&endpoint);
    let checkpoint = h.dir.path().join("watch.json");
    let worker = std::thread::spawn(move || {
        let mut socket = wire::accept(&listener);
        wire::start(&mut socket);
        wire::send(
            &mut socket,
            wire::event("product", "BOOKMARK", "journal-start"),
        );
        let mut event = wire::event("product", "ADDED", "journal-not-fully-emitted");
        event["payload"]["data"]["watchProducts"]["product"]["body"] = json!("x".repeat(524288));
        wire::send(&mut socket, event);
    });
    let mut child = h
        .credential_cmd("synthetic-watch-token")
        .args([
            "--timeout",
            "10s",
            "watch",
            "product",
            "--checkpoint-file",
            checkpoint.to_str().unwrap(),
        ])
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .unwrap();
    let deadline = Instant::now() + Duration::from_secs(6);
    loop {
        if child.try_wait().unwrap().is_some() {
            break;
        }
        if Instant::now() >= deadline {
            child.kill().unwrap();
            child.wait().unwrap();
            panic!("Slow observer exceeded its bounded output deadline");
        }
        std::thread::sleep(Duration::from_millis(10));
    }
    let out = child.wait_with_output().unwrap();
    worker.join().unwrap();
    assert_eq!(out.status.code(), Some(8), "{}", stderr(&out));
    let error: Value = serde_json::from_slice(&out.stderr).unwrap();
    assert_eq!(error["code"], "WATCH_BACKPRESSURE");
    let record: Value =
        serde_json::from_str(&std::fs::read_to_string(checkpoint).unwrap()).unwrap();
    assert_eq!(record["resourceVersion"], "journal-start");
}

#[test]
fn revoked_auth_fails_without_dumping_reason_and_interrupt_closes_the_connection() {
    use tungstenite::{
        Message,
        protocol::{CloseFrame, frame::coding::CloseCode},
    };
    let (listener, endpoint) = wire::listener();
    let h = configured(&endpoint);
    let checkpoint = h.dir.path().join("watch.json");
    let worker = std::thread::spawn(move || {
        let mut socket = wire::accept(&listener);
        let init = wire::read(&mut socket);
        assert_eq!(init["type"], "connection_init");
        socket
            .send(Message::Close(Some(CloseFrame {
                code: CloseCode::from(4403),
                reason: "denied synthetic-watch-token".into(),
            })))
            .unwrap();
    });
    let denied = invoke(&h, &checkpoint, &["product"]);
    worker.join().unwrap();
    assert_eq!(denied.status.code(), Some(3), "{}", stderr(&denied));
    assert!(lines(&denied).is_empty());
    let (listener, endpoint) = wire::listener();
    let h = configured(&endpoint);
    let checkpoint = h.dir.path().join("watch.json");
    let worker = std::thread::spawn(move || {
        let mut socket = wire::accept(&listener);
        wire::start(&mut socket);
        wire::send(
            &mut socket,
            wire::event("product", "BOOKMARK", "journal-start"),
        );
        assert!(
            socket.read().is_err(),
            "Interrupted client must close its only subscription connection"
        );
    });
    let mut child = h
        .credential_cmd("synthetic-watch-token")
        .args([
            "--timeout",
            "10s",
            "watch",
            "product",
            "--checkpoint-file",
            checkpoint.to_str().unwrap(),
        ])
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .unwrap();
    let deadline = Instant::now() + Duration::from_secs(3);
    while !checkpoint.exists() {
        if Instant::now() >= deadline {
            child.kill().unwrap();
            let out = child.wait_with_output().unwrap();
            panic!("No initial checkpoint: {}", stderr(&out));
        }
        std::thread::sleep(Duration::from_millis(10));
    }
    assert_eq!(
        unsafe { libc::kill(child.id() as libc::pid_t, libc::SIGINT) },
        0
    );
    assert!(!child.wait_with_output().unwrap().status.success());
    worker.join().unwrap();
    assert!(!h.dir.path().join("gitstored.sock").exists());
}

#[test]
fn unsupported_selections_and_unbounded_inputs_fail_before_configuration_or_io() {
    let h = Harness::new();
    h.write("config.toml", "invalid configuration must not be read");
    for (arguments, code) in [
        (vec!["watch", "collection"], 6),
        (vec!["watch", "product", "--id", "opaque"], 6),
        (vec!["watch", "product", "-o", "yaml"], 2),
        (
            vec!["watch", "product", "--selector", r#"{"unknown":true}"#],
            2,
        ),
        (
            vec![
                "watch",
                "product",
                "--resource-version",
                "__namespace_watch_bootstrap__",
            ],
            2,
        ),
    ] {
        assert_eq!(h.run(&arguments).status.code(), Some(code), "{arguments:?}");
    }
    assert!(!h.dir.path().join("gitstore/watch").exists());
}

#[test]
fn disconnect_before_first_bookmark_requires_explicit_restart() {
    let (listener, endpoint) = wire::listener();
    let h = configured(&endpoint);
    let checkpoint = h.dir.path().join("watch.json");
    let worker = std::thread::spawn(move || {
        let mut socket = wire::accept(&listener);
        wire::start(&mut socket);
        drop(socket);
        std::thread::sleep(Duration::from_millis(250));
        assert!(
            matches!(listener.accept(),Err(error) if error.kind()==std::io::ErrorKind::WouldBlock),
            "No hidden fresh-watch fallback is allowed after losing an uncheckpointed stream"
        );
    });
    let out = invoke(&h, &checkpoint, &["product"]);
    worker.join().unwrap();
    assert_eq!(out.status.code(), Some(8));
    assert!(lines(&out).is_empty());
    assert_eq!(
        serde_json::from_slice::<Value>(&out.stderr).unwrap()["code"],
        "WATCH_GAP"
    );
    assert!(!checkpoint.exists());
}
