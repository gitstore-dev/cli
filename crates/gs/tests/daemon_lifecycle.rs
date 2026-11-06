//! Actual gs processes against a controlled protocol peer. Native workload,
//! durable-store and crash proofs are separate C/Linux runtime scenarios.
use gs::daemon_protocol::{read_frame, write_frame};
use serde_json::{Value, json};
use std::{
    collections::BTreeMap,
    fs,
    io::Read,
    os::unix::{
        fs::PermissionsExt,
        net::{UnixListener, UnixStream},
    },
    path::{Path, PathBuf},
    process::{Child, Command, Output, Stdio},
    sync::mpsc,
    thread,
    time::{Duration, Instant},
};
const BOUND: Duration = Duration::from_secs(5);
struct Fixture {
    root: tempfile::TempDir,
    socket: PathBuf,
    listener: UnixListener,
}
impl Fixture {
    fn new() -> Self {
        let root = tempfile::tempdir().unwrap();
        let base = root.path().canonicalize().unwrap();
        fs::set_permissions(&base, fs::Permissions::from_mode(0o700)).unwrap();
        let work = base.join("work");
        fs::create_dir(&work).unwrap();
        git2::Repository::init(&work).unwrap();
        fs::write(work.join("uncommitted"), "preserve my working tree\n").unwrap();
        fs::write(
            base.join("remote.toml"),
            "invalid unrelated remote configuration {{{",
        )
        .unwrap();
        let socket = base.join("manager.sock");
        let listener = UnixListener::bind(&socket).unwrap();
        fs::set_permissions(&socket, fs::Permissions::from_mode(0o600)).unwrap();
        listener.set_nonblocking(true).unwrap();
        Self {
            root,
            socket,
            listener,
        }
    }
    fn command(&self, args: &[&str]) -> Command {
        let base = self.root.path().canonicalize().unwrap();
        let mut c = Command::new(env!("CARGO_BIN_EXE_gs"));
        c.env_clear()
            .env("PATH", "/nonexistent")
            .env("HOME", &base)
            .env("XDG_CONFIG_HOME", base.join("config"))
            .env("XDG_STATE_HOME", base.join("state"))
            .env("XDG_CACHE_HOME", base.join("cache"))
            .env("GITSTORE_CONFIG", base.join("remote.toml"))
            .env("GITSTORE_CONTEXT", "unrelated-remote")
            .current_dir(base.join("work"))
            .args([
                "--daemon-socket",
                self.socket.to_str().unwrap(),
                "--non-interactive",
                "-o",
                "json",
            ])
            .args(args)
            .stdout(Stdio::piped())
            .stderr(Stdio::piped());
        c
    }
    fn files(&self) -> BTreeMap<PathBuf, Vec<u8>> {
        fn visit(root: &Path, path: &Path, files: &mut BTreeMap<PathBuf, Vec<u8>>) {
            for item in fs::read_dir(path).unwrap() {
                let p = item.unwrap().path();
                let kind = fs::symlink_metadata(&p).unwrap().file_type();
                if kind.is_dir() {
                    visit(root, &p, files);
                } else if kind.is_file() {
                    files.insert(
                        p.strip_prefix(root).unwrap().to_owned(),
                        fs::read(p).unwrap(),
                    );
                }
            }
        }
        let mut files = BTreeMap::new();
        visit(self.root.path(), self.root.path(), &mut files);
        files
    }
}
fn accept(listener: &UnixListener) -> UnixStream {
    let deadline = Instant::now() + BOUND;
    loop {
        match listener.accept() {
            Ok((s, _)) => {
                s.set_nonblocking(false).unwrap();
                s.set_read_timeout(Some(BOUND)).unwrap();
                s.set_write_timeout(Some(BOUND)).unwrap();
                return s;
            }
            Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                assert!(Instant::now() < deadline, "controlled peer accept deadline");
                thread::sleep(Duration::from_millis(2));
            }
            Err(e) => panic!("accept: {e}"),
        }
    }
}
fn request(s: &mut UnixStream) -> Value {
    read_frame(s).unwrap().expect("request")
}
fn respond(s: &mut UnixStream, req: &Value, result: Value) {
    write_frame(
        s,
        &json!({"version":{"major":1,"minor":0},"requestId":req["requestId"],"result":result}),
    )
    .unwrap();
}
fn prepare(listener: &UnixListener) -> UnixStream {
    prepare_capabilities(listener, false)
}
fn negotiate_capabilities(listener: &UnixListener, logs: bool, oidc: bool) -> UnixStream {
    let mut s = accept(listener);
    let req = request(&mut s);
    assert_eq!(req["method"], "system.negotiate");
    let examples: Vec<Value> = serde_json::from_slice(
        &fs::read(
            Path::new(env!("CARGO_MANIFEST_DIR")).join("../../tests/fixtures/daemon/examples.json"),
        )
        .unwrap(),
    )
    .unwrap();
    let limits = examples
        .iter()
        .find(|x| x["example"] == "EX-001" && x["kind"] == "response")
        .unwrap()["message"]["result"]["limits"]
        .clone();
    let mut capabilities = json!([
        "deployment.start",
        "deployment.stop",
        "deployment.validate",
        "deployment.read",
        "operation.read",
        "operation.cancel"
    ]);
    if oidc {
        capabilities
            .as_array_mut()
            .unwrap()
            .push(json!("deployment.oidc"));
    }
    if logs {
        capabilities
            .as_array_mut()
            .unwrap()
            .push(json!("stream.read"));
    }
    respond(
        &mut s,
        &req,
        json!({"selectedVersion":{"major":1,"minor":0},"capabilities":capabilities,"limits":limits}),
    );
    s
}
fn prepare_capabilities(listener: &UnixListener, logs: bool) -> UnixStream {
    let mut s = negotiate_capabilities(listener, logs, true);
    let req = request(&mut s);
    assert_eq!(req["method"], "deployment.validate");
    respond(
        &mut s,
        &req,
        json!({"diagnostics":[],"requiredAssets":[],"effectivePlan":{"assetManifestDigest":format!("sha256:{}", "a".repeat(64))}}),
    );
    s
}
fn wait(mut child: Child) -> Output {
    // Drain both pipes while waiting: a foreground log plus cleanup diagnostics
    // can fill a small host pipe and block the child before it exits.
    let mut stdout = child.stdout.take().unwrap();
    let mut stderr = child.stderr.take().unwrap();
    let stdout = thread::spawn(move || {
        let mut bytes = Vec::new();
        stdout.read_to_end(&mut bytes).unwrap();
        bytes
    });
    let stderr = thread::spawn(move || {
        let mut bytes = Vec::new();
        stderr.read_to_end(&mut bytes).unwrap();
        bytes
    });
    let end = Instant::now() + BOUND;
    let mut timed_out = false;
    let status = loop {
        if let Some(status) = child.try_wait().unwrap() {
            break status;
        }
        if Instant::now() >= end {
            child.kill().unwrap();
            timed_out = true;
            break child.wait().unwrap();
        }
        thread::sleep(Duration::from_millis(5));
    };
    let output = Output {
        status,
        stdout: stdout.join().unwrap(),
        stderr: stderr.join().unwrap(),
    };
    assert!(
        !timed_out,
        "gs process exceeded controlled test bound: {}",
        String::from_utf8_lossy(&output.stderr)
    );
    output
}
fn diagnostic(output: &Output) -> Value {
    output
        .stderr
        .split(|x| *x == b'\n')
        .rev()
        .find_map(|line| serde_json::from_slice::<Value>(line).ok())
        .expect("structured CLI error")
}
#[test]
fn lost_ack_rerun_preserves_key_intent_and_original_operation() {
    let f = Fixture::new();
    let original = f.files();
    let listener = f.listener.try_clone().unwrap();
    let peer = thread::spawn(move || {
        let mut first = prepare(&listener);
        let accepted = request(&mut first);
        assert_eq!(accepted["method"], "deployment.start");
        assert_eq!(accepted["idempotencyKey"], "original-key");
        drop(first); // Controlled durable acceptance, ACK intentionally lost.
        let mut second = prepare(&listener);
        let replay = request(&mut second);
        assert_eq!(replay["params"], accepted["params"]);
        assert_eq!(replay["idempotencyKey"], accepted["idempotencyKey"]);
        respond(
            &mut second,
            &replay,
            json!({"operationId":"original-operation","deploymentId":"local","state":"accepted","replayed":true}),
        );
        assert!(read_frame(&mut second).unwrap().is_none());
        let mut third = prepare(&listener);
        let conflict = request(&mut third);
        assert_eq!(conflict["idempotencyKey"], accepted["idempotencyKey"]);
        assert_ne!(conflict["params"], accepted["params"]);
        write_frame(&mut third, &json!({"version":{"major":1,"minor":0},"requestId":conflict["requestId"],"error":{"code":"IDEMPOTENCY_CONFLICT","stage":"accept","retryable":false,"message":"Different original intent"}})).unwrap();
    });
    let args = [
        "serve",
        "--deployment",
        "local",
        "--detach",
        "--idempotency-key",
        "original-key",
    ];
    let lost = wait(f.command(&args).spawn().unwrap());
    assert!(!lost.status.success() && lost.stdout.is_empty());
    assert_eq!(diagnostic(&lost)["idempotencyKey"], "original-key");
    let replay = wait(f.command(&args).spawn().unwrap());
    assert!(
        replay.status.success(),
        "{}",
        String::from_utf8_lossy(&replay.stderr)
    );
    let result: Value = serde_json::from_slice(&replay.stdout).unwrap();
    assert_eq!(result["operationId"], "original-operation");
    assert_eq!(result["replayed"], true);
    let changed = wait(
        f.command(&[
            "api",
            "serve",
            "--deployment",
            "local",
            "--detach",
            "--idempotency-key",
            "original-key",
        ])
        .spawn()
        .unwrap(),
    );
    assert_eq!(diagnostic(&changed)["code"], "IDEMPOTENCY_CONFLICT");
    peer.join().unwrap();
    assert_eq!(f.files(), original);
}
#[test]
fn detached_ack_timeout_does_not_submit_cancellation() {
    let f = Fixture::new();
    let original = f.files();
    let listener = f.listener.try_clone().unwrap();
    let peer = thread::spawn(move || {
        let mut s = prepare(&listener);
        let accepted = request(&mut s);
        assert_eq!(accepted["method"], "deployment.start");
        thread::sleep(Duration::from_millis(400));
        assert!(read_frame(&mut s).unwrap().is_none());
        let mut s = prepare(&listener);
        let replay = request(&mut s);
        assert_eq!(replay["idempotencyKey"], accepted["idempotencyKey"]);
        assert_eq!(replay["params"], accepted["params"]);
        respond(
            &mut s,
            &replay,
            json!({"operationId":"detached-operation","deploymentId":"local","state":"running","replayed":true}),
        );
        assert!(read_frame(&mut s).unwrap().is_none());
    });
    let args = [
        "serve",
        "--deployment",
        "local",
        "--detach",
        "--idempotency-key",
        "detached-key",
    ];
    let mut command = f.command(&args);
    command.args(["--timeout", "150ms"]);
    let timed_out = wait(command.spawn().unwrap());
    assert_eq!(timed_out.status.code(), Some(7));
    assert_eq!(diagnostic(&timed_out)["idempotencyKey"], "detached-key");
    let replay = wait(f.command(&args).spawn().unwrap());
    assert!(replay.status.success());
    let result: Value = serde_json::from_slice(&replay.stdout).unwrap();
    assert_eq!(result["operationId"], "detached-operation");
    assert_eq!(result["state"], "running");
    peer.join().unwrap();
    assert_eq!(f.files(), original);
}
#[test]
fn foreground_interrupt_cancels_active_start_or_stops_its_completed_generation() {
    for (terminal, logs) in [(false, false), (true, false), (false, true), (true, true)] {
        let f = Fixture::new();
        let original = f.files();
        let listener = f.listener.try_clone().unwrap();
        let (tx, rx) = mpsc::channel();
        let peer = thread::spawn(move || {
            let mut s = prepare_capabilities(&listener, logs);
            let start = request(&mut s);
            assert_eq!(start["method"], "deployment.start");
            respond(
                &mut s,
                &start,
                json!({"operationId":"start-operation","deploymentId":"local","state":"accepted"}),
            );
            let mut notified = false;
            loop {
                let req = request(&mut s);
                match req["method"].as_str().unwrap() {
                    "stream.subscribe" => {
                        assert!(logs);
                        respond(
                            &mut s,
                            &req,
                            json!({"subscriptionId":"foreground-logs","replayAvailable":true}),
                        );
                        write_frame(&mut s,&json!({"version":{"major":1,"minor":0},"subscriptionId":"foreground-logs","deploymentId":"local","sequence":1,"type":"log","payload":{"text":"foreground stderr fixture","component":"api","cursor":"fixture-cursor"}})).unwrap();
                    }
                    "operation.inspect" => respond(
                        &mut s,
                        &req,
                        json!({"operationId":"start-operation","deploymentId":"local","state":if terminal {"succeeded"} else {"running"}}),
                    ),
                    "deployment.inspect" => {
                        respond(
                            &mut s,
                            &req,
                            json!({"deploymentId":"local","lastOperationId":"start-operation","desiredState":"running","readiness":if terminal {"ready"} else {"pending"},"recovery":"normal","generation":1,"observedGeneration":1}),
                        );
                        if !notified {
                            tx.send(()).unwrap();
                            notified = true;
                        }
                    }
                    "operation.cancel" | "deployment.stop" => {
                        assert_eq!(
                            req["method"],
                            if terminal {
                                "deployment.stop"
                            } else {
                                "operation.cancel"
                            }
                        );
                        assert_ne!(req["idempotencyKey"], start["idempotencyKey"]);
                        if terminal {
                            assert_eq!(req["params"]["expectedOperationId"], "start-operation");
                        } else {
                            assert_eq!(req["params"]["operationId"], "start-operation");
                        }
                        respond(
                            &mut s,
                            &req,
                            json!({"operationId":"cleanup-operation","deploymentId":"local","state":"accepted"}),
                        );
                        break;
                    }
                    other => panic!("Unexpected method: {other}"),
                }
            }
            let req = request(&mut s);
            assert_eq!(req["method"], "operation.wait");
            assert_eq!(req["params"]["operationId"], "cleanup-operation");
            respond(
                &mut s,
                &req,
                json!({"operationId":"cleanup-operation","deploymentId":"local","state":"succeeded","result":{"stopped":true}}),
            );
            assert!(read_frame(&mut s).unwrap().is_none());
        });
        let child = f
            .command(&[
                "serve",
                "--deployment",
                "local",
                "--idempotency-key",
                "foreground-key",
            ])
            .spawn()
            .unwrap();
        rx.recv_timeout(BOUND).unwrap();
        // Direct child only, after its signal handler and foreground observation exist.
        assert_eq!(unsafe { libc::kill(child.id() as i32, libc::SIGINT) }, 0);
        let output = wait(child);
        assert!(
            output.status.success(),
            "{}",
            String::from_utf8_lossy(&output.stderr)
        );
        assert!(String::from_utf8_lossy(&output.stderr).contains("Owned cleanup observed"));
        if logs {
            assert!(String::from_utf8_lossy(&output.stderr).contains("foreground stderr fixture"));
            assert!(!String::from_utf8_lossy(&output.stdout).contains("foreground stderr fixture"));
        }
        peer.join().unwrap();
        assert_eq!(f.files(), original);
    }
}

#[test]
fn oidc_selections_preserve_standalone_and_combined_intent() {
    for (args, components) in [
        (vec!["oidc", "serve"], json!(["oidc"])),
        (
            vec!["api", "serve", "--add", "oidc"],
            json!(["api", "oidc"]),
        ),
        (
            vec!["serve", "--add", "oidc"],
            json!(["api", "git", "controller", "oidc"]),
        ),
    ] {
        let fixture = Fixture::new();
        let before = fixture.files();
        let mut args = args;
        args.extend(["--deployment", "local", "--allow-provision", "--detach"]);
        let child = fixture.command(&args).spawn().unwrap();
        let mut peer = prepare(&fixture.listener);
        let start = request(&mut peer);
        assert_eq!(start["method"], "deployment.start");
        assert_eq!(start["params"]["profileId"], "core");
        assert_eq!(start["params"]["profileVersion"], "2");
        assert_eq!(start["params"]["components"], components);
        respond(
            &mut peer,
            &start,
            json!({"operationId":"oidc-start","deploymentId":"local","state":"accepted"}),
        );
        assert!(wait(child).status.success());
        assert_eq!(fixture.files(), before);
    }
}

#[test]
fn oidc_missing_capability_fails_before_validation_or_submission() {
    let fixture = Fixture::new();
    let before = fixture.files();
    let child = fixture
        .command(&[
            "oidc",
            "serve",
            "--deployment",
            "local",
            "--allow-provision",
            "--detach",
        ])
        .spawn()
        .unwrap();
    let mut peer = negotiate_capabilities(&fixture.listener, false, false);
    let output = wait(child);
    assert_eq!(output.status.code(), Some(6));
    assert!(String::from_utf8_lossy(&output.stderr).contains("UNSUPPORTED_CAPABILITY"));
    assert!(read_frame(&mut peer).unwrap().is_none());
    assert_eq!(fixture.files(), before);
}
