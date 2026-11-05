//! Migration CLI control semantics against an owner-authenticated protocol peer.
//! Native datastore/helper and persistent graph checks remain separate.
use gs::daemon_protocol::{read_frame, write_frame};
use serde_json::{Value, json};
use std::{
    collections::BTreeMap,
    fs,
    os::unix::{
        fs::PermissionsExt,
        net::{UnixListener, UnixStream},
    },
    path::{Path, PathBuf},
    process::{Child, Command, Output, Stdio},
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
fn negotiate(listener: &UnixListener, enabled: bool) -> UnixStream {
    let mut stream = accept(listener);
    let req = request(&mut stream);
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
        .find(|item| item["example"] == "EX-001" && item["kind"] == "response")
        .unwrap()["message"]["result"]["limits"]
        .clone();
    let capabilities = if enabled {
        json!(["migration.start", "operation.read"])
    } else {
        json!(["operation.read"])
    };
    respond(
        &mut stream,
        &req,
        json!({"selectedVersion":{"major":1,"minor":0},"capabilities":capabilities,"limits":limits}),
    );
    stream
}
fn wait(mut child: Child) -> Output {
    let end = Instant::now() + BOUND;
    while child.try_wait().unwrap().is_none() {
        if Instant::now() >= end {
            child.kill().unwrap();
            child.wait().unwrap();
            panic!("migration CLI exceeded controlled bound");
        }
        thread::sleep(Duration::from_millis(5));
    }
    child.wait_with_output().unwrap()
}
fn diagnostic(output: &Output) -> Value {
    output
        .stderr
        .split(|byte| *byte == b'\n')
        .rev()
        .find_map(|line| serde_json::from_slice::<Value>(line).ok())
        .unwrap()
}
#[test]
fn backup_and_deployment_selection_fail_before_connecting() {
    let fixture = Fixture::new();
    let before = fixture.files();
    for arguments in [
        vec!["migrate", "--deployment", "local", "--target", "registered"],
        vec!["migrate", "--target", "registered", "--backup-acknowledged"],
        vec![
            "migrate",
            "--deployment",
            "local",
            "--target",
            "registered",
            "--backup-acknowledged",
            "--datastore",
            "memdb",
        ],
    ] {
        let output = wait(fixture.command(&arguments).spawn().unwrap());
        assert_eq!(output.status.code(), Some(2));
        assert!(output.stdout.is_empty());
        assert_eq!(diagnostic(&output)["code"], "INVALID_INPUT");
    }
    assert_eq!(
        fixture.listener.accept().unwrap_err().kind(),
        std::io::ErrorKind::WouldBlock
    );
    assert_eq!(fixture.files(), before);
}
#[test]
fn missing_capability_never_submits_a_migration() {
    let fixture = Fixture::new();
    let listener = fixture.listener.try_clone().unwrap();
    let peer = thread::spawn(move || {
        let mut stream = negotiate(&listener, false);
        assert!(read_frame(&mut stream).unwrap().is_none());
    });
    let output = wait(
        fixture
            .command(&[
                "migrate",
                "--deployment",
                "local",
                "--target",
                "registered",
                "--backup-acknowledged",
                "--detach",
            ])
            .spawn()
            .unwrap(),
    );
    assert!(!output.status.success());
    assert_eq!(diagnostic(&output)["code"], "UNSUPPORTED_CAPABILITY");
    peer.join().unwrap();
}
#[test]
fn lost_ack_replays_same_target_backup_and_key_without_retrying_automatically() {
    let fixture = Fixture::new();
    let before = fixture.files();
    let listener = fixture.listener.try_clone().unwrap();
    let peer = thread::spawn(move || {
        let mut first = negotiate(&listener, true);
        let original = request(&mut first);
        assert_eq!(original["method"], "migration.start");
        assert_eq!(original["idempotencyKey"], "migration-key");
        assert_eq!(
            original["params"],
            json!({"deploymentId":"local","targetVersion":"registered","backupAcknowledged":true})
        );
        drop(first);
        let mut second = negotiate(&listener, true);
        let replay = request(&mut second);
        assert_eq!(replay["params"], original["params"]);
        assert_eq!(replay["idempotencyKey"], original["idempotencyKey"]);
        respond(
            &mut second,
            &replay,
            json!({"operationId":"original-migration","deploymentId":"local","state":"accepted","replayed":true}),
        );
        assert!(read_frame(&mut second).unwrap().is_none());
    });
    let args = [
        "migrate",
        "--deployment",
        "local",
        "--target",
        "registered",
        "--backup-acknowledged",
        "--detach",
        "--idempotency-key",
        "migration-key",
    ];
    let lost = wait(fixture.command(&args).spawn().unwrap());
    assert!(!lost.status.success());
    assert_eq!(diagnostic(&lost)["idempotencyKey"], "migration-key");
    let replay = wait(fixture.command(&args).spawn().unwrap());
    assert!(replay.status.success());
    let accepted: Value = serde_json::from_slice(&replay.stdout).unwrap();
    assert_eq!(accepted["operationId"], "original-migration");
    assert_eq!(accepted["idempotencyKey"], "migration-key");
    peer.join().unwrap();
    assert_eq!(fixture.files(), before);
}
#[test]
fn terminal_failure_and_observation_loss_preserve_identity_without_cancellation() {
    for (deadline, terminal_code) in [
        (false, "OWNERSHIP_UNCERTAIN"),
        (false, "MIGRATION_PRECONDITION"),
        (true, "DEADLINE_EXCEEDED"),
    ] {
        let fixture = Fixture::new();
        let listener = fixture.listener.try_clone().unwrap();
        let peer = thread::spawn(move || {
            let mut stream = negotiate(&listener, true);
            let start = request(&mut stream);
            assert_eq!(start["method"], "migration.start");
            let key = start["idempotencyKey"].as_str().unwrap().to_owned();
            assert!(key.starts_with("gs-"));
            respond(
                &mut stream,
                &start,
                json!({"operationId":"known-migration","deploymentId":"local","state":"accepted","replayed":false}),
            );
            let observe = request(&mut stream);
            assert_eq!(observe["method"], "operation.wait");
            assert_eq!(
                observe["params"],
                json!({"operationId":"known-migration","until":"terminal"})
            );
            if deadline {
                write_frame(&mut stream, &json!({"version":{"major":1,"minor":0},"requestId":observe["requestId"],"error":{"code":"DEADLINE_EXCEEDED","stage":"wait","retryable":false,"message":"Observation deadline"}})).unwrap();
            } else {
                respond(
                    &mut stream,
                    &observe,
                    json!({"operationId":"known-migration","state":"failed","recovery":"uncertain","error":{"code":terminal_code}}),
                );
            }
            assert!(read_frame(&mut stream).unwrap().is_none());
            key
        });
        let output = wait(
            fixture
                .command(&[
                    "migrate",
                    "--deployment",
                    "local",
                    "--target",
                    "registered",
                    "--backup-acknowledged",
                ])
                .spawn()
                .unwrap(),
        );
        assert!(!output.status.success());
        let error = diagnostic(&output);
        assert_eq!(error["operationId"], "known-migration");
        assert_eq!(error["deploymentId"], "local");
        assert_eq!(error["idempotencyKey"], peer.join().unwrap());
        assert_eq!(error["code"], terminal_code);
        if deadline {
            assert!(output.stdout.is_empty());
        } else {
            assert_eq!(
                serde_json::from_slice::<Value>(&output.stdout).unwrap()["state"],
                "failed"
            );
        }
    }
}
