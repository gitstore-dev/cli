#![cfg(unix)]
use gs::{
    credentials::{Secret, Store},
    runtime_credentials::{Identity, Request, resolve},
};
use serde_json::json;
use std::{cell::RefCell, fs, os::unix::fs::PermissionsExt};

#[derive(Default)]
struct Synthetic(RefCell<Vec<String>>);
impl Store for Synthetic {
    fn read(&self, key: &str) -> gs::error::Result<Option<Secret>> {
        self.0.borrow_mut().push(key.into());
        Secret::new("fixture-only".into()).map(Some)
    }
    fn remove(&self, _: &str) -> gs::error::Result<bool> {
        panic!("broker cannot remove credentials")
    }
}
fn identity() -> serde_json::Value {
    json!({"installation":"local-fixture", "issuer":"fixture-issuer", "account":"fixture-account",
        "purpose":"registry-pull", "registry":"https://ghcr.io", "repository":"gitstore-dev/api"})
}
fn bootstrap_request(source: serde_json::Value, candidate: &str) -> Vec<u8> {
    serde_json::to_vec(
        &json!({"identity":{"installation":"isolated-runtime-fixture",
        "issuer":"core/1", "account":"fixture-deployment", "purpose":"bootstrap-material"},
        "source":source,"createIfAbsent":candidate}),
    )
    .unwrap()
}
#[test]
fn explicit_file_bootstrap_is_add_only_and_concurrent_creators_reuse_one_value() {
    let root = tempfile::tempdir().unwrap();
    fs::set_permissions(root.path(), fs::Permissions::from_mode(0o700)).unwrap();
    let path = root.path().join("bootstrap.json");
    let source = json!({"kind":"protected-file","path":path});
    let first = bootstrap_request(source.clone(), "synthetic-material-one");
    let second = bootstrap_request(source.clone(), "synthetic-material-two");
    let a = std::thread::spawn(move || {
        resolve(&Request::decode(&first).unwrap(), &Synthetic::default())
            .unwrap()
            .unwrap()
            .expose()
            .to_owned()
    });
    let b = std::thread::spawn(move || {
        resolve(&Request::decode(&second).unwrap(), &Synthetic::default())
            .unwrap()
            .unwrap()
            .expose()
            .to_owned()
    });
    let actual = a.join().unwrap();
    assert_eq!(actual, b.join().unwrap());
    assert_eq!(fs::read_to_string(&path).unwrap(), actual);
    assert_eq!(
        fs::metadata(&path).unwrap().permissions().mode() & 0o777,
        0o600
    );
    let again = bootstrap_request(source, "replacement-must-not-be-stored");
    assert_eq!(
        resolve(&Request::decode(&again).unwrap(), &Synthetic::default())
            .unwrap()
            .unwrap()
            .expose(),
        actual
    );
    assert_eq!(fs::read_dir(root.path()).unwrap().count(), 2);
}
#[test]
fn bootstrap_creation_rejects_other_purposes_links_and_shared_parent_before_writes() {
    let store = Synthetic::default();
    let forbidden = json!({"identity":identity(),"source":{"kind":"os-store"},
        "createIfAbsent":"must-not-replace-registry-auth"});
    assert!(Request::decode(&serde_json::to_vec(&forbidden).unwrap()).is_err());
    assert!(store.0.borrow().is_empty());
    let root = tempfile::tempdir().unwrap();
    let path = root.path().join("bootstrap");
    fs::set_permissions(root.path(), fs::Permissions::from_mode(0o755)).unwrap();
    let request = Request::decode(&bootstrap_request(
        json!({"kind":"protected-file","path":path}),
        "fixture-only",
    ))
    .unwrap();
    assert!(resolve(&request, &store).is_err());
    assert!(!path.exists());
    fs::set_permissions(root.path(), fs::Permissions::from_mode(0o700)).unwrap();
    let actual = root.path().join("existing");
    fs::write(&actual, "fixture-retained").unwrap();
    fs::set_permissions(&actual, fs::Permissions::from_mode(0o600)).unwrap();
    std::os::unix::fs::symlink(&actual, &path).unwrap();
    assert!(resolve(&request, &store).is_err());
    assert_eq!(fs::read_to_string(actual).unwrap(), "fixture-retained");
}
#[test]
fn exact_identity_separates_purposes_accounts_installations_and_repositories() {
    let base = identity();
    let original: Identity = serde_json::from_value(base.clone()).unwrap();
    let mut keys = std::collections::HashSet::from([original.key().unwrap()]);
    for (field, value) in [
        ("installation", "another"),
        ("issuer", "another"),
        ("account", "another"),
        ("repository", "gitstore-dev/controller"),
        ("registry", "https://registry.example.org"),
    ] {
        let mut changed = base.clone();
        changed[field] = value.into();
        let id: Identity = serde_json::from_value(changed).unwrap();
        assert!(keys.insert(id.key().unwrap()));
    }
    for purpose in [
        "bootstrap-user",
        "controller-enrollment",
        "controller-signing",
    ] {
        let mut changed = base.clone();
        changed["purpose"] = purpose.into();
        changed.as_object_mut().unwrap().remove("registry");
        changed.as_object_mut().unwrap().remove("repository");
        let id: Identity = serde_json::from_value(changed).unwrap();
        assert!(keys.insert(id.key().unwrap()));
    }
    assert!(original.key().unwrap().contains("runtime-credentials-v1"));
}
#[test]
fn invalid_scope_fails_before_any_store_access_and_never_falls_back() {
    let store = Synthetic::default();
    for (field, value) in [
        ("registry", "https://ghcr.io.evil/path"),
        ("registry", "http://ghcr.io"),
        ("registry", "https://user:secret@ghcr.io"),
        ("repository", "../api"),
        ("repository", "gitstore-dev//api"),
        ("purpose", "graphql"),
        ("account", ""),
    ] {
        let mut id = identity();
        id[field] = value.into();
        let req = Request::decode(
            &serde_json::to_vec(&json!({"identity":id,"source":{"kind":"os-store"}})).unwrap(),
        );
        assert!(req.and_then(|r| resolve(&r, &store)).is_err());
    }
    assert!(store.0.borrow().is_empty());
    let request = Request::decode(
        &serde_json::to_vec(&json!({"identity":identity(),"source":{"kind":"os-store"}})).unwrap(),
    )
    .unwrap();
    assert_eq!(
        resolve(&request, &store).unwrap().unwrap().expose(),
        "fixture-only"
    );
    assert_eq!(store.0.borrow().len(), 1);
    let malformed = br#"{"identity":{},"identity":{},"source":{"kind":"os-store"}}"#;
    assert!(Request::decode(malformed).is_err());
    for source in [
        json!({"kind":"os-store","path":"/not-allowed"}),
        json!({"kind":"protected-file","path":"/not-allowed","extra":true}),
    ] {
        assert!(
            Request::decode(
                &serde_json::to_vec(&json!({"identity":identity(),"source":source})).unwrap()
            )
            .is_err()
        );
    }
}
#[test]
fn explicit_file_is_bounded_private_and_does_not_consult_os_store() {
    let root = tempfile::tempdir().unwrap();
    let file = root.path().join("credential");
    fs::write(&file, "synthetic-secret\n").unwrap();
    fs::set_permissions(&file, fs::Permissions::from_mode(0o600)).unwrap();
    let store = Synthetic::default();
    let request = Request::decode(
        &serde_json::to_vec(&json!({"identity":identity(),
        "source":{"kind":"protected-file", "path":file}}))
        .unwrap(),
    )
    .unwrap();
    assert_eq!(
        resolve(&request, &store).unwrap().unwrap().expose(),
        "synthetic-secret"
    );
    fs::set_permissions(&file, fs::Permissions::from_mode(0o644)).unwrap();
    assert!(resolve(&request, &store).is_err());
    fs::set_permissions(&file, fs::Permissions::from_mode(0o600)).unwrap();
    fs::write(&file, vec![b'x'; 16385]).unwrap();
    assert!(resolve(&request, &store).is_err());
    fs::remove_file(&file).unwrap();
    std::os::unix::fs::symlink("missing", &file).unwrap();
    assert!(resolve(&request, &store).is_err());
    assert!(store.0.borrow().is_empty());
}

#[test]
fn installed_entry_returns_secret_only_on_inherited_descriptor() {
    use std::{
        io::{Read, Write},
        os::{
            fd::AsRawFd,
            unix::{net::UnixStream, process::CommandExt},
        },
        process::{Command, Stdio},
        time::Duration,
    };
    let root = tempfile::tempdir().unwrap();
    let file = root.path().join("secret");
    fs::write(&file, "private-fixture-value").unwrap();
    fs::set_permissions(&file, fs::Permissions::from_mode(0o600)).unwrap();
    let (mut parent, child) = UnixStream::pair().unwrap();
    parent
        .set_read_timeout(Some(Duration::from_secs(5)))
        .unwrap();
    let child_fd = child.as_raw_fd();
    let mut command = Command::new(env!("CARGO_BIN_EXE_gs"));
    command
        .env_clear()
        .env("PATH", "/nonexistent")
        .env("HOME", root.path())
        .arg("__runtime-credential-broker-v1")
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped());
    unsafe {
        command.pre_exec(move || {
            if libc::dup2(child_fd, 3) < 0 || libc::fcntl(3, libc::F_SETFD, 0) < 0 {
                return Err(std::io::Error::last_os_error());
            }
            Ok(())
        });
    }
    let process = command.spawn().unwrap();
    drop(child);
    let request = serde_json::to_vec(
        &json!({"identity":identity(),"source":{"kind":"protected-file","path":file}}),
    )
    .unwrap();
    parent
        .write_all(&(request.len() as u32).to_be_bytes())
        .unwrap();
    parent.write_all(&request).unwrap();
    parent.shutdown(std::net::Shutdown::Write).unwrap();
    let mut response = Vec::new();
    parent.read_to_end(&mut response).unwrap();
    assert_eq!(&response[..5], &[0, 0, 0, 0, 21]);
    assert_eq!(&response[5..], b"private-fixture-value");
    let output = process.wait_with_output().unwrap();
    assert!(output.status.success());
    assert!(output.stdout.is_empty() && output.stderr.is_empty());
    let output = Command::new(env!("CARGO_BIN_EXE_gs"))
        .env_clear()
        .env("HOME", root.path())
        .arg("__runtime-credential-broker-v1")
        .output()
        .unwrap();
    assert!(!output.status.success());
    assert!(output.stdout.is_empty() && output.stderr.is_empty());
}
