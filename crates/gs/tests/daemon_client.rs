use gs::{
    daemon_client::Client,
    daemon_protocol::{read_frame, write_frame},
};
use serde_json::{Value, json};
use std::{
    fs,
    io::Write,
    os::unix::{
        fs::PermissionsExt,
        net::{UnixListener, UnixStream},
    },
    path::{Path, PathBuf},
    thread,
    time::{Duration, Instant},
};
use tempfile::TempDir;
const WAIT: Duration = Duration::from_secs(2);
fn fixture() -> (TempDir, PathBuf, UnixListener) {
    let root = tempfile::tempdir().unwrap();
    fs::set_permissions(root.path(), fs::Permissions::from_mode(0o700)).unwrap();
    let path = root.path().canonicalize().unwrap().join("manager.sock");
    let listener = UnixListener::bind(&path).unwrap();
    fs::set_permissions(&path, fs::Permissions::from_mode(0o600)).unwrap();
    listener.set_nonblocking(true).unwrap();
    (root, path, listener)
}
fn accept(listener: &UnixListener) -> UnixStream {
    let deadline = Instant::now() + WAIT;
    loop {
        match listener.accept() {
            Ok((s, _)) => {
                s.set_nonblocking(false).unwrap();
                s.set_read_timeout(Some(WAIT)).unwrap();
                s.set_write_timeout(Some(WAIT)).unwrap();
                return s;
            }
            Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                assert!(Instant::now() < deadline, "accept deadline");
                thread::sleep(Duration::from_millis(2));
            }
            Err(e) => panic!("accept: {e}"),
        }
    }
}
fn request(s: &mut UnixStream) -> Value {
    read_frame(s).unwrap().expect("request frame")
}
fn respond(s: &mut UnixStream, id: &Value, result: Value) {
    write_frame(
        s,
        &json!({"version":{"major":1,"minor":0},"requestId":id,"result":result}),
    )
    .unwrap();
}
fn handshake(s: &mut UnixStream, caps: &[&str]) {
    let req = request(s);
    assert_eq!(req["method"], "system.negotiate");
    let path =
        Path::new(env!("CARGO_MANIFEST_DIR")).join("../../tests/fixtures/daemon/examples.json");
    let examples: Vec<Value> = serde_json::from_slice(&fs::read(path).unwrap()).unwrap();
    let limits = examples
        .iter()
        .find(|v| v["example"] == "EX-001" && v["kind"] == "response")
        .unwrap()["message"]["result"]["limits"]
        .clone();
    respond(
        s,
        &req["requestId"],
        json!({"selectedVersion":{"major":1,"minor":0},"capabilities":caps,"limits":limits}),
    );
}
#[test]
fn matches_out_of_order_responses_to_active_requests() {
    let (_root, path, listener) = fixture();
    let server = thread::spawn(move || {
        let mut s = accept(&listener);
        handshake(&mut s, &["system.read"]);
        let first = request(&mut s);
        let second = request(&mut s);
        respond(&mut s, &second["requestId"], json!({"order":2}));
        respond(&mut s, &first["requestId"], json!({"order":1}));
    });
    let mut client = Client::connect(&path, &["system.read"], WAIT).unwrap();
    let first = client
        .submit("system.capabilities", json!({}), None, WAIT)
        .unwrap();
    let second = client
        .submit("system.capabilities", json!({}), None, WAIT)
        .unwrap();
    assert_eq!(client.receive(&first).unwrap()["order"], 1);
    assert_eq!(client.receive(&second).unwrap()["order"], 2);
    drop(client);
    server.join().unwrap();
}
#[test]
fn missing_capability_fails_before_mutation() {
    let (_root, path, listener) = fixture();
    let server = thread::spawn(move || {
        let mut s = accept(&listener);
        handshake(&mut s, &["system.read"]);
        assert!(read_frame(&mut s).unwrap().is_none());
    });
    let mut client = Client::connect(&path, &[], WAIT).unwrap();
    let error = client
        .call(
            "deployment.start",
            json!({"deploymentId":"d","profileId":"core","profileVersion":"v1"}),
            Some("same"),
            WAIT,
        )
        .unwrap_err();
    assert_eq!(error.code, "UNSUPPORTED_CAPABILITY");
    drop(client);
    server.join().unwrap();
}
#[test]
fn replay_after_lost_acknowledgment_preserves_original_key() {
    let (_root, path, listener) = fixture();
    let server = thread::spawn(move || {
        let mut s = accept(&listener);
        handshake(&mut s, &["deployment.start"]);
        let first = request(&mut s);
        assert_eq!(first["idempotencyKey"], "original-key");
        drop(s);
        let mut s = accept(&listener);
        handshake(&mut s, &["deployment.start"]);
        let replay = request(&mut s);
        assert_eq!(replay["idempotencyKey"], first["idempotencyKey"]);
        assert_eq!(replay["params"], first["params"]);
        respond(
            &mut s,
            &replay["requestId"],
            json!({"operationId":"original-operation","state":"accepted"}),
        );
    });
    let params = json!({"deploymentId":"d","profileId":"core","profileVersion":"v1"});
    let mut client = Client::connect(&path, &["deployment.start"], WAIT).unwrap();
    let error = client
        .call(
            "deployment.start",
            params.clone(),
            Some("original-key"),
            WAIT,
        )
        .unwrap_err();
    assert_eq!(error.idempotency_key.as_deref(), Some("original-key"));
    drop(client);
    let result = Client::replay(
        &path,
        &["deployment.start"],
        "deployment.start",
        params,
        "original-key",
        WAIT,
    )
    .unwrap();
    assert_eq!(result["operationId"], "original-operation");
    server.join().unwrap();
}
#[test]
fn unknown_response_identity_cannot_satisfy_pending_request() {
    let (_root, path, listener) = fixture();
    let server = thread::spawn(move || {
        let mut s = accept(&listener);
        handshake(&mut s, &["system.read"]);
        let _ = request(&mut s);
        respond(&mut s, &json!("unknown"), json!({}));
    });
    let mut client = Client::connect(&path, &[], WAIT).unwrap();
    assert_eq!(
        client
            .call("system.capabilities", json!({}), None, WAIT)
            .unwrap_err()
            .code,
        "INVALID_ARGUMENT"
    );
    server.join().unwrap();
}
#[test]
fn trickled_bytes_do_not_extend_observation_deadline() {
    let (_root, path, listener) = fixture();
    let server = thread::spawn(move || {
        let mut s = accept(&listener);
        handshake(&mut s, &["system.read"]);
        let r = request(&mut s);
        let bytes = serde_json::to_vec(
            &json!({"version":{"major":1,"minor":0},"requestId":r["requestId"],"result":{}}),
        )
        .unwrap();
        if s.write_all(&(bytes.len() as u32).to_be_bytes()).is_err() {
            return;
        }
        for b in bytes {
            if s.write_all(&[b]).is_err() {
                break;
            }
            thread::sleep(Duration::from_millis(10));
        }
    });
    let mut client = Client::connect(&path, &[], WAIT).unwrap();
    let start = Instant::now();
    let error = client
        .call(
            "system.capabilities",
            json!({}),
            None,
            Duration::from_millis(60),
        )
        .unwrap_err();
    assert_eq!(error.code, "DEADLINE_EXCEEDED");
    assert!(start.elapsed() < Duration::from_millis(500));
    drop(client);
    server.join().unwrap();
}
#[test]
fn subscription_events_do_not_consume_request_identity() {
    let (_root, path, listener) = fixture();
    let server = thread::spawn(move || {
        let mut s = accept(&listener);
        handshake(&mut s, &["system.read", "stream.read"]);
        let subscribe = request(&mut s);
        respond(
            &mut s,
            &subscribe["requestId"],
            json!({"subscriptionId":"sub1","replayAvailable":true}),
        );
        let read = request(&mut s);
        write_frame(&mut s,&json!({"version":{"major":1,"minor":0},"subscriptionId":"sub1","deploymentId":"d","sequence":1,"type":"log.chunk","payload":{"text":"synthetic"}})).unwrap();
        respond(&mut s, &read["requestId"], json!({"observed":true}));
    });
    let mut client = Client::connect(&path, &[], WAIT).unwrap();
    client
        .call(
            "stream.subscribe",
            json!({"deploymentId":"d","kind":"logs"}),
            None,
            WAIT,
        )
        .unwrap();
    assert_eq!(
        client
            .call("system.capabilities", json!({}), None, WAIT)
            .unwrap()["observed"],
        true
    );
    assert_eq!(client.next_event().unwrap()["sequence"], 1);
    assert!(client.next_event().is_none());
    server.join().unwrap();
}
#[test]
fn rejects_unsafe_socket_permissions_and_symlink_paths() {
    let (_root, path, _listener) = fixture();
    fs::set_permissions(&path, fs::Permissions::from_mode(0o666)).unwrap();
    assert_eq!(
        Client::connect(&path, &[], WAIT).err().unwrap().code,
        "UNAUTHORIZED_PEER"
    );
    fs::set_permissions(&path, fs::Permissions::from_mode(0o600)).unwrap();
    let link = path.parent().unwrap().join("alias.sock");
    std::os::unix::fs::symlink(&path, &link).unwrap();
    assert_eq!(
        Client::connect(&link, &[], WAIT).err().unwrap().code,
        "UNAUTHORIZED_PEER"
    );
}

#[test]
fn incompatible_version_is_actionable_before_any_operation() {
    let (_root, path, listener) = fixture();
    let server = thread::spawn(move || {
        let mut socket = accept(&listener);
        let req = request(&mut socket);
        write_frame(&mut socket,&json!({"version":{"major":1,"minor":0},"requestId":req["requestId"],"error":{
            "code":"INCOMPATIBLE_VERSION","stage":"negotiate","retryable":false,"message":"Use a matching CLI and daemon"}})).unwrap();
        assert!(read_frame(&mut socket).unwrap().is_none());
    });
    let error = Client::connect(&path, &["deployment.validate"], WAIT)
        .err()
        .unwrap();
    assert_eq!(error.code, "INCOMPATIBLE_VERSION");
    assert_eq!(error.exit, 6);
    assert!(!error.retryable);
    server.join().unwrap();
}
