//! Installed per-user manager startup. No PATH search, downloads or privilege changes.
use crate::daemon_client::{Client, ClientError};
use std::{
    fs::{self, DirBuilder, File, OpenOptions},
    io::Read,
    os::fd::{AsRawFd, FromRawFd},
    os::unix::{
        fs::{DirBuilderExt, FileExt, MetadataExt, OpenOptionsExt},
        net::UnixStream,
        process::CommandExt,
    },
    path::{Component, Path, PathBuf},
    process::{Command, Stdio},
    time::{Duration, Instant},
};
fn failure(code: &str, message: &str) -> Box<ClientError> {
    Box::new(ClientError {
        code: code.into(),
        stage: "resolve".into(),
        message: message.into(),
        exit: if code == "LOCAL_TRANSPORT" {
            8
        } else {
            crate::daemon_protocol::ProtocolError::code_exit_category(code)
        },
        retryable: false,
        operation_id: None,
        deployment_id: None,
        idempotency_key: None,
    })
}
pub(crate) fn directory(path: &Path) -> Result<(), Box<ClientError>> {
    if !path.is_absolute() {
        return Err(failure(
            "INVALID_ARGUMENT",
            "Manager directories must be absolute",
        ));
    }
    let uid = unsafe { libc::geteuid() };
    let mut current = PathBuf::new();
    for component in path.components() {
        if !matches!(component, Component::RootDir | Component::Normal(_)) {
            return Err(failure("INVALID_ARGUMENT", "Unsafe manager directory path"));
        }
        current.push(component.as_os_str());
        let metadata = match fs::symlink_metadata(&current) {
            Ok(m) => m,
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => {
                match DirBuilder::new().mode(0o700).create(&current) {
                    Ok(()) => {}
                    Err(e) if e.kind() == std::io::ErrorKind::AlreadyExists => {}
                    Err(_) => {
                        return Err(failure(
                            "UNAUTHORIZED_PEER",
                            "Cannot create private manager directory",
                        ));
                    }
                };
                fs::symlink_metadata(&current)
                    .map_err(|_| failure("UNAUTHORIZED_PEER", "Cannot verify manager directory"))?
            }
            Err(_) => {
                return Err(failure(
                    "UNAUTHORIZED_PEER",
                    "Cannot inspect manager directory",
                ));
            }
        };
        if !metadata.is_dir()
            || metadata.file_type().is_symlink()
            || (metadata.uid() != 0 && metadata.uid() != uid)
            || (metadata.mode() & 0o022 != 0
                && !(metadata.uid() == 0 && metadata.mode() & 0o1000 != 0))
        {
            return Err(failure(
                "UNAUTHORIZED_PEER",
                "Manager directory ownership or permissions are unsafe",
            ));
        }
    }
    let metadata = fs::symlink_metadata(path).map_err(|_| {
        failure(
            "UNAUTHORIZED_PEER",
            "Cannot verify private manager directory",
        )
    })?;
    if metadata.uid() != uid || metadata.mode() & 0o7777 != 0o700 {
        return Err(failure(
            "UNAUTHORIZED_PEER",
            "Manager directory must be owned and mode 0700",
        ));
    }
    Ok(())
}
fn installed() -> Result<(PathBuf, File), Box<ClientError>> {
    let own =
        std::env::current_exe().map_err(|_| failure("NOT_FOUND", "Cannot locate installed gs"))?;
    installed_at(&own)
}
fn installed_at(own: &Path) -> Result<(PathBuf, File), Box<ClientError>> {
    let path = own
        .parent()
        .ok_or_else(|| failure("NOT_FOUND", "Cannot locate installed manager"))?
        .join("gitstored");
    let file = OpenOptions::new()
        .read(true)
        .custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC)
        .open(&path)
        .map_err(|_| {
            failure(
                "NOT_FOUND",
                "Install matching gs and gitstored executables in the same directory",
            )
        })?;
    let m = file
        .metadata()
        .map_err(|_| failure("UNAUTHORIZED_PEER", "Cannot verify installed manager"))?;
    let uid = unsafe { libc::geteuid() };
    let mut magic = [0u8; 4];
    if !m.is_file()
        || m.nlink() != 1
        || (m.uid() != 0 && m.uid() != uid)
        || m.mode() & 0o022 != 0
        || m.mode() & 0o111 == 0
        || m.len() > 134217728
        || file.read_at(&mut magic, 0).ok() != Some(4)
        || magic != *b"\x7fELF"
    {
        return Err(failure(
            "UNAUTHORIZED_PEER",
            "Installed manager executable identity is unsafe",
        ));
    }
    Ok((path, file))
}
fn matching(mut client: Client, timeout: Duration) -> Result<Client, Box<ClientError>> {
    let capabilities = client.call("system.capabilities", serde_json::json!({}), None, timeout)?;
    if capabilities["implementationVersion"] != env!("CARGO_PKG_VERSION") {
        return Err(failure(
            "INCOMPATIBLE_VERSION",
            "Install matching gs and gitstored versions",
        ));
    }
    Ok(client)
}
/// `automatic` is true only for the standard per-user socket and an explicit
/// lifecycle invocation. Explicit socket selection always means connect-only.
pub fn connect(
    path: &Path,
    required: &[&str],
    timeout: Duration,
    automatic: bool,
) -> Result<Client, Box<ClientError>> {
    match Client::connect(path, required, timeout) {
        Ok(client) => {
            return if automatic {
                matching(client, timeout)
            } else {
                Ok(client)
            };
        }
        Err(error)
            if !automatic || !matches!(error.code.as_str(), "NOT_FOUND" | "LOCAL_TRANSPORT") =>
        {
            return Err(error);
        }
        Err(_) => {}
    }
    if !cfg!(target_os = "linux") {
        return Err(failure(
            "UNSUPPORTED_HOST",
            "Automatic local runtime startup requires native Linux and its per-user session",
        ));
    }
    let uid = unsafe { libc::geteuid() };
    if uid == 0 {
        return Err(failure(
            "UNSUPPORTED_HOST",
            "Run the local manager as the invoking non-root user",
        ));
    }
    let canonical = PathBuf::from(format!("/run/user/{uid}"));
    let runtime = std::env::var_os("XDG_RUNTIME_DIR")
        .map(PathBuf::from)
        .ok_or_else(|| {
            failure(
                "UNSUPPORTED_HOST",
                "A per-user Linux runtime session is required",
            )
        })?;
    if runtime != canonical || path != runtime.join("gitstore/gitstored.sock") {
        return Err(failure(
            "UNSUPPORTED_HOST",
            "Select an explicit daemon socket for a nonstandard runtime directory",
        ));
    }
    let home = std::env::var_os("HOME")
        .map(PathBuf::from)
        .filter(|p| p.is_absolute())
        .ok_or_else(|| failure("NOT_FOUND", "Select an absolute user home"))?;
    let state_base = std::env::var_os("XDG_STATE_HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|| home.join(".local/state"));
    let state = state_base.join("gitstore");
    let (executable, pinned) = installed()?;
    directory(&runtime)?;
    directory(path.parent().expect("standard socket has parent"))?;
    directory(&state)?;
    // Preserve the pinned executable above the fixed private readiness FD.
    let executable_fd = unsafe { libc::fcntl(pinned.as_raw_fd(), libc::F_DUPFD_CLOEXEC, 10) };
    if executable_fd < 10 {
        return Err(failure(
            "RESOURCE_LIMIT",
            "Cannot pin manager startup executable",
        ));
    }
    let _executable_handle = unsafe { File::from_raw_fd(executable_fd) };
    let (mut readiness, child_ready) = UnixStream::pair().map_err(|_| {
        failure(
            "RESOURCE_LIMIT",
            "Cannot create private manager readiness channel",
        )
    })?;
    readiness
        .set_nonblocking(true)
        .map_err(|_| failure("LOCAL_TRANSPORT", "Cannot bound private readiness channel"))?;
    let ready_fd = child_ready.as_raw_fd();
    let start = Instant::now();
    let bound = timeout.min(Duration::from_secs(10));
    let mut command = Command::new(format!("/proc/self/fd/{executable_fd}"));
    command
        .arg0(&executable)
        .args(["--runtime-dir"])
        .arg(path.parent().expect("standard socket has parent"))
        .arg("--state-dir")
        .arg(&state)
        .args(["--ready-fd", "3"])
        .env_clear()
        .env("PATH", "/nonexistent")
        .env("LANG", "C")
        .env("HOME", home)
        .env("XDG_RUNTIME_DIR", &runtime)
        .env(
            "DBUS_SESSION_BUS_ADDRESS",
            format!("unix:path=/run/user/{uid}/bus"),
        )
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null());
    unsafe {
        command.pre_exec(move || {
            if libc::setsid() < 0
                || libc::dup2(ready_fd, 3) != 3
                || libc::fcntl(3, libc::F_SETFD, 0) < 0
            {
                return Err(std::io::Error::last_os_error());
            }
            Ok(())
        });
    }
    let mut child = command
        .spawn()
        .map_err(|_| failure("NOT_FOUND", "Cannot execute the installed per-user manager"))?;
    drop(child_ready);
    let mut ready_bytes = Vec::new();
    let mut private_ready = false;
    let mut competing = false;
    loop {
        let Some(remaining) = bound.checked_sub(start.elapsed()).filter(|v| !v.is_zero()) else {
            return Err(failure(
                "DEADLINE_EXCEEDED",
                "Manager readiness deadline expired; retry observation before starting another operation",
            ));
        };
        if !private_ready && !competing {
            let mut bytes = [0u8; 256];
            match readiness.read(&mut bytes) {
                Ok(0) => {}
                Ok(count) => {
                    if ready_bytes.len() + count > 512 {
                        return Err(failure(
                            "MALFORMED_FRAME",
                            "Manager readiness exceeded its bound",
                        ));
                    }
                    ready_bytes.extend_from_slice(&bytes[..count]);
                    if ready_bytes.last() == Some(&b'\n') {
                        let ready: serde_json::Value = serde_json::from_slice(&ready_bytes)
                            .map_err(|_| {
                                failure("MALFORMED_FRAME", "Invalid private manager readiness")
                            })?;
                        if ready["ready"] != true
                            || ready["implementationVersion"] != env!("CARGO_PKG_VERSION")
                            || ready["protocol"] != serde_json::json!({"major":1,"minor":0})
                        {
                            return Err(failure(
                                "INCOMPATIBLE_VERSION",
                                "Installed manager readiness version does not match gs",
                            ));
                        }
                        private_ready = true;
                    }
                }
                Err(error)
                    if matches!(
                        error.kind(),
                        std::io::ErrorKind::WouldBlock | std::io::ErrorKind::Interrupted
                    ) => {}
                Err(_) => {
                    return Err(failure(
                        "LOCAL_TRANSPORT",
                        "Private manager readiness failed",
                    ));
                }
            }
        }
        if private_ready || competing {
            match Client::connect(path, required, remaining.min(Duration::from_millis(100))) {
                Ok(client) => return matching(client, remaining),
                Err(error) if !matches!(error.code.as_str(), "NOT_FOUND" | "LOCAL_TRANSPORT") => {
                    return Err(error);
                }
                Err(_) => {}
            }
        }
        if let Some(status) = child
            .try_wait()
            .map_err(|_| failure("LOCAL_TRANSPORT", "Cannot observe manager startup"))?
        {
            // A concurrent startup may own the lifetime lock and be preparing its
            // socket. Observe it through the same authenticated handshake.
            if status.code() != Some(5) {
                return Err(failure(
                    "LOCAL_TRANSPORT",
                    "Installed manager exited before readiness; inspect owner settings and session prerequisites",
                ));
            }
            competing = true;
        }
        std::thread::sleep(Duration::from_millis(10));
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::os::unix::fs::{PermissionsExt, symlink};
    #[test]
    fn private_setup_rejects_linked_or_exposed_directories() {
        let fixture = tempfile::tempdir().unwrap();
        let path = fixture
            .path()
            .canonicalize()
            .unwrap()
            .join("state/gitstore");
        directory(&path).unwrap();
        assert_eq!(fs::metadata(&path).unwrap().mode() & 0o7777, 0o700);
        fs::set_permissions(&path, fs::Permissions::from_mode(0o755)).unwrap();
        assert!(directory(&path).is_err());
        fs::set_permissions(&path, fs::Permissions::from_mode(0o700)).unwrap();
        symlink(&path, fixture.path().join("linked")).unwrap();
        assert!(directory(&fixture.path().join("linked")).is_err());
    }
    #[test]
    fn installed_manager_requires_pinned_private_executable_identity() {
        let fixture = tempfile::tempdir().unwrap();
        let own = fixture.path().join("gs");
        let daemon = fixture.path().join("gitstored");
        fs::write(&daemon, b"\x7fELFsynthetic-testing-header").unwrap();
        fs::set_permissions(&daemon, fs::Permissions::from_mode(0o500)).unwrap();
        assert!(installed_at(&own).is_ok());
        fs::hard_link(&daemon, fixture.path().join("duplicate")).unwrap();
        assert!(installed_at(&own).is_err());
        fs::remove_file(fixture.path().join("duplicate")).unwrap();
        fs::set_permissions(&daemon, fs::Permissions::from_mode(0o777)).unwrap();
        assert!(installed_at(&own).is_err());
        fs::remove_file(&daemon).unwrap();
        symlink("missing", &daemon).unwrap();
        assert!(installed_at(&own).is_err());
        fs::remove_file(&daemon).unwrap();
        fs::write(&daemon, b"#!/bin/sh\n").unwrap();
        fs::set_permissions(&daemon, fs::Permissions::from_mode(0o500)).unwrap();
        assert!(installed_at(&own).is_err());
    }
}
