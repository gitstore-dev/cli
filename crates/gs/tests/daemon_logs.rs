//! Real gs processes against an isolated owner-authenticated stream peer.
use gs::daemon_protocol::{read_frame, write_frame};
use serde_json::{Value, json};
use std::{
    fs,
    io::{BufRead, BufReader, Read, Write},
    os::unix::{
        fs::PermissionsExt,
        net::{UnixListener, UnixStream},
    },
    path::Path,
    process::{Child, Command, Stdio},
    sync::mpsc,
    thread,
    time::{Duration, Instant},
};
const BOUND: Duration = Duration::from_secs(5);
fn response(stream: &mut UnixStream, request: &Value, result: Value) {
    write_frame(
        stream,
        &json!({"version":{"major":1,"minor":0},"requestId":request["requestId"],"result":result}),
    )
    .unwrap();
}
fn negotiate(listener: UnixListener) -> UnixStream {
    let (mut stream, _) = listener.accept().unwrap();
    stream.set_read_timeout(Some(BOUND)).unwrap();
    stream.set_write_timeout(Some(BOUND)).unwrap();
    let request = read_frame(&mut stream).unwrap().unwrap();
    assert_eq!(request["method"], "system.negotiate");
    let examples: Vec<Value> = serde_json::from_slice(
        &fs::read(
            Path::new(env!("CARGO_MANIFEST_DIR")).join("../../tests/fixtures/daemon/examples.json"),
        )
        .unwrap(),
    )
    .unwrap();
    let limits = examples
        .iter()
        .find(|v| v["example"] == "EX-001" && v["kind"] == "response")
        .unwrap()["message"]["result"]["limits"]
        .clone();
    response(
        &mut stream,
        &request,
        json!({"selectedVersion":{"major":1,"minor":0},"capabilities":["stream.read"],"limits":limits}),
    );
    stream
}
fn handshake(listener: UnixListener) -> UnixStream {
    let mut stream = negotiate(listener);
    let request = read_frame(&mut stream).unwrap().unwrap();
    assert_eq!(request["method"], "stream.subscribe");
    assert_eq!(request["params"]["deploymentId"], "fixture");
    response(
        &mut stream,
        &request,
        json!({"subscriptionId":"fixture-subscription","replayAvailable":true}),
    );
    stream
}
fn event(sequence: u32, text: &str) -> Value {
    json!({"version":{"major":1,"minor":0},"subscriptionId":"fixture-subscription","deploymentId":"fixture","sequence":sequence,"type":"log","payload":{"cursor":format!("cursor-{sequence}"),"component":"api","observedAt":"2026-11-04T00:00:00Z","text":text}})
}
fn unsubscribe(stream: &mut UnixStream) {
    let request = read_frame(stream).unwrap().unwrap();
    assert_eq!(request["method"], "stream.unsubscribe");
    assert_eq!(request["params"]["subscriptionId"], "fixture-subscription");
    response(stream, &request, json!({"closed":true}));
    assert!(read_frame(stream).unwrap().is_none());
}
fn wait(child: &mut Child) {
    let deadline = Instant::now() + BOUND;
    loop {
        if let Some(status) = child.try_wait().unwrap() {
            assert!(status.success());
            break;
        }
        if Instant::now() >= deadline {
            child.kill().unwrap();
            panic!("log process exit deadline");
        }
        thread::sleep(Duration::from_millis(5));
    }
}
fn fixture(json_output: bool) -> (tempfile::TempDir, UnixListener, Child) {
    let root = tempfile::tempdir().unwrap();
    let base = root.path().canonicalize().unwrap();
    fs::set_permissions(&base, fs::Permissions::from_mode(0o700)).unwrap();
    let socket = base.join("daemon.sock");
    let listener = UnixListener::bind(&socket).unwrap();
    fs::set_permissions(&socket, fs::Permissions::from_mode(0o600)).unwrap();
    let mut command = Command::new(env!("CARGO_BIN_EXE_gs"));
    command
        .env_clear()
        .env("HOME", &base)
        .env("XDG_CONFIG_HOME", base.join("config"))
        .env("PATH", "/nonexistent")
        .arg("--daemon-socket")
        .arg(&socket)
        .arg("--non-interactive");
    if json_output {
        command.args(["-o", "json"]);
    }
    let child = command
        .args(["deployment", "logs", "fixture"])
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .unwrap();
    (root, listener, child)
}
#[test]
fn fragmented_text_and_json_logs_unsubscribe_on_interrupt() {
    for json_output in [false, true] {
        let (_root, listener, mut child) = fixture(json_output);
        let peer = thread::spawn(move || {
            let mut stream = handshake(listener);
            let value = event(1, "first line");
            let mut frame = Vec::new();
            write_frame(&mut frame, &value).unwrap();
            stream.write_all(&frame[..2]).unwrap();
            thread::sleep(Duration::from_millis(175));
            stream.write_all(&frame[2..]).unwrap();
            unsubscribe(&mut stream);
        });
        let stdout = child.stdout.take().unwrap();
        let (sent, received) = mpsc::channel();
        let reader = thread::spawn(move || {
            let mut reader = BufReader::new(stdout);
            let mut line = String::new();
            reader.read_line(&mut line).unwrap();
            sent.send(line).unwrap();
            let mut remainder = String::new();
            reader.read_to_string(&mut remainder).unwrap();
            remainder
        });
        let line = received.recv_timeout(BOUND).unwrap();
        if json_output {
            let value: Value = serde_json::from_str(&line).unwrap();
            assert_eq!(value["payload"]["text"], "first line");
            assert_eq!(value["subscriptionId"], "fixture-subscription");
        } else {
            assert_eq!(line, "first line\n");
        }
        assert_eq!(
            unsafe { libc::kill(child.id() as libc::pid_t, libc::SIGINT) },
            0
        );
        wait(&mut child);
        assert!(reader.join().unwrap().is_empty());
        let mut stderr = String::new();
        child
            .stderr
            .take()
            .unwrap()
            .read_to_string(&mut stderr)
            .unwrap();
        assert!(stderr.is_empty(), "{stderr}");
        peer.join().unwrap();
    }
}
#[test]
fn interrupt_is_responsive_when_log_output_is_blocked() {
    let (_root, listener, mut child) = fixture(true);
    let (sent, ready) = mpsc::channel();
    let peer = thread::spawn(move || {
        let mut stream = handshake(listener);
        sent.send(()).unwrap();
        for sequence in 1..=12 {
            write_frame(&mut stream, &event(sequence, &"x".repeat(16384))).unwrap();
        }
        unsubscribe(&mut stream);
    });
    ready.recv_timeout(BOUND).unwrap();
    thread::sleep(Duration::from_millis(200));
    let interrupted = Instant::now();
    assert_eq!(
        unsafe { libc::kill(child.id() as libc::pid_t, libc::SIGINT) },
        0
    );
    wait(&mut child);
    assert!(interrupted.elapsed() < Duration::from_secs(3));
    peer.join().unwrap();
}
#[test]
fn invalid_log_formats_fail_before_connecting() {
    for format in ["yaml", "table"] {
        let result = Command::new(env!("CARGO_BIN_EXE_gs"))
            .args([
                "--daemon-socket",
                "/nonexistent/manager.sock",
                "-o",
                format,
                "deployment",
                "logs",
                "fixture",
            ])
            .output()
            .unwrap();
        assert_eq!(result.status.code(), Some(2));
        assert!(result.stdout.is_empty());
    }
}

#[test]
fn interrupt_during_pending_subscription_closes_observation() {
    let (_root, listener, mut child) = fixture(true);
    let (sent, ready) = mpsc::channel();
    let peer = thread::spawn(move || {
        let mut stream = negotiate(listener);
        let request = read_frame(&mut stream).unwrap().unwrap();
        assert_eq!(request["method"], "stream.subscribe");
        sent.send(()).unwrap();
        assert!(read_frame(&mut stream).unwrap().is_none());
    });
    ready.recv_timeout(BOUND).unwrap();
    assert_eq!(
        unsafe { libc::kill(child.id() as libc::pid_t, libc::SIGINT) },
        0
    );
    wait(&mut child);
    peer.join().unwrap();
}
