//! Private runtime credential broker. No remote context or ambient registry auth.
use crate::{
    credentials::{Secret, Store},
    error::{Error, Result},
};
use serde::Deserialize;
use std::{
    io::{Read, Write},
    path::PathBuf,
};
use zeroize::Zeroizing;

pub const REQUEST_LIMIT: usize = 8192;
pub const SECRET_LIMIT: usize = 16384;
const ENTRY: &str = "__runtime-credential-broker-v1";
fn denied() -> Error {
    Error::auth("Runtime credential unavailable or invalid")
}

#[derive(Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum Purpose {
    RegistryPull,
    BootstrapUser,
    ControllerEnrollment,
    ControllerSigning,
    BootstrapMaterial,
}
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Identity {
    installation: String,
    issuer: String,
    account: String,
    purpose: Purpose,
    registry: Option<String>,
    repository: Option<String>,
}
impl Identity {
    pub fn key(&self) -> Result<String> {
        for label in [&self.installation, &self.issuer, &self.account] {
            if label.is_empty() || label.len() > 256 || label.chars().any(char::is_control) {
                return Err(denied());
            }
        }
        let purpose = match self.purpose {
            Purpose::RegistryPull => "registry-pull",
            Purpose::BootstrapUser => "bootstrap-user",
            Purpose::ControllerEnrollment => "controller-enrollment",
            Purpose::ControllerSigning => "controller-signing",
            Purpose::BootstrapMaterial => "bootstrap-material",
        };
        if matches!(self.purpose, Purpose::RegistryPull) {
            let origin = self.registry.as_deref().ok_or_else(denied)?;
            let url = url::Url::parse(origin).map_err(|_| denied())?;
            if url.scheme() != "https"
                || !url.username().is_empty()
                || url.password().is_some()
                || url.path() != "/"
                || url.query().is_some()
                || url.fragment().is_some()
                || url.origin().ascii_serialization() != origin
            {
                return Err(denied());
            }
            let repository = self.repository.as_deref().ok_or_else(denied)?;
            if repository.is_empty()
                || repository.len() > 256
                || repository.split('/').any(|part| {
                    part.is_empty()
                        || !part.as_bytes()[0].is_ascii_alphanumeric()
                        || !part.as_bytes()[part.len() - 1].is_ascii_alphanumeric()
                        || !part.bytes().all(|b| {
                            b.is_ascii_lowercase() || b.is_ascii_digit() || b"._-".contains(&b)
                        })
                })
            {
                return Err(denied());
            }
        } else if self.registry.is_some() || self.repository.is_some() {
            return Err(denied());
        }
        // Distinct versioned domain prevents collisions with GraphQL/Git keys.
        Ok(serde_json::json!([
            "runtime-credentials-v1",
            self.installation,
            self.issuer,
            self.account,
            purpose,
            self.registry,
            self.repository
        ])
        .to_string())
    }
}
#[derive(Deserialize)]
#[serde(tag = "kind", rename_all = "kebab-case", deny_unknown_fields)]
enum Source {
    OsStore {},
    ProtectedFile { path: PathBuf },
}
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Request {
    identity: Identity,
    source: Source,
    #[serde(rename = "createIfAbsent", default, deserialize_with = "candidate")]
    create_if_absent: Option<Zeroizing<String>>,
}
fn candidate<'de, D: serde::Deserializer<'de>>(
    decoder: D,
) -> std::result::Result<Option<Zeroizing<String>>, D::Error> {
    Option::<String>::deserialize(decoder).map(|value| value.map(Zeroizing::new))
}
impl Request {
    pub fn decode(bytes: &[u8]) -> Result<Self> {
        if bytes.is_empty() || bytes.len() > REQUEST_LIMIT {
            return Err(denied());
        }
        // Struct decoding rejects duplicate/unknown fields without reflecting input.
        let request: Self = serde_json::from_slice(bytes).map_err(|_| denied())?;
        request.identity.key()?;
        if let Some(value) = &request.create_if_absent
            && (!matches!(request.identity.purpose, Purpose::BootstrapMaterial)
                || value.is_empty()
                || value.len() > SECRET_LIMIT
                || value.chars().any(char::is_control))
        {
            return Err(denied());
        }
        Ok(request)
    }
}
pub fn resolve(request: &Request, store: &impl Store) -> Result<Option<Secret>> {
    let key = request.identity.key()?;
    let mut creation_guard = None;
    if let Some(value) = &request.create_if_absent {
        match &request.source {
            Source::OsStore {} => {
                let candidate = Secret::new(value.to_string()).map_err(|_| denied())?;
                if store.read(&key)?.is_none() {
                    store.insert_runtime_if_absent(&key, &candidate)?;
                }
            }
            Source::ProtectedFile { path } => {
                creation_guard = Some(create_protected(path, value)?);
            }
        }
    }
    let result = match &request.source {
        Source::OsStore {} => store.read(&key)?,
        Source::ProtectedFile { path } => {
            use std::os::unix::fs::{MetadataExt, OpenOptionsExt};
            if !path.is_absolute() {
                return Err(denied());
            }
            let file = match std::fs::OpenOptions::new()
                .read(true)
                .custom_flags(libc::O_CLOEXEC | libc::O_NOFOLLOW | libc::O_NONBLOCK)
                .open(path)
            {
                Ok(file) => file,
                Err(error)
                    if error.kind() == std::io::ErrorKind::NotFound
                        && matches!(request.identity.purpose, Purpose::BootstrapMaterial) =>
                {
                    bootstrap_parent(path)?;
                    return Ok(None);
                }
                Err(_) => return Err(denied()),
            };
            let before = file.metadata().map_err(|_| denied())?;
            if !before.is_file()
                || before.uid() != unsafe { libc::geteuid() }
                || before.mode() & 0o077 != 0
                || before.nlink() != 1
                || before.len() == 0
                || before.len() > SECRET_LIMIT as u64
            {
                return Err(denied());
            }
            let mut bytes = Zeroizing::new(Vec::new());
            (&file)
                .take((SECRET_LIMIT + 1) as u64)
                .read_to_end(&mut bytes)
                .map_err(|_| denied())?;
            let after = file.metadata().map_err(|_| denied())?;
            if bytes.len() > SECRET_LIMIT
                || before.dev() != after.dev()
                || before.ino() != after.ino()
                || before.len() != after.len()
                || before.mode() != after.mode()
                || before.uid() != after.uid()
                || before.nlink() != after.nlink()
                || before.mtime() != after.mtime()
                || before.mtime_nsec() != after.mtime_nsec()
                || before.ctime() != after.ctime()
                || before.ctime_nsec() != after.ctime_nsec()
            {
                return Err(denied());
            }
            let text = std::str::from_utf8(&bytes).map_err(|_| denied())?;
            Some(Secret::new(text.to_owned()).map_err(|_| denied())?)
        }
    };
    if result
        .as_ref()
        .is_some_and(|s| s.expose().len() > SECRET_LIMIT)
    {
        return Err(denied());
    }
    drop(creation_guard);
    Ok(result)
}
fn bootstrap_parent(path: &std::path::Path) -> Result<PathBuf> {
    use std::os::unix::fs::MetadataExt;
    if !path.is_absolute() {
        return Err(denied());
    }
    let parent = path.parent().ok_or_else(denied)?;
    let metadata = std::fs::symlink_metadata(parent).map_err(|_| denied())?;
    if !metadata.is_dir()
        || metadata.uid() != unsafe { libc::geteuid() }
        || metadata.mode() & 0o7777 != 0o700
    {
        return Err(denied());
    }
    parent.canonicalize().map_err(|_| denied())
}
fn create_protected(path: &std::path::Path, value: &str) -> Result<std::fs::File> {
    use std::os::unix::fs::{MetadataExt, OpenOptionsExt};
    let canonical = bootstrap_parent(path)?;
    let destination = canonical.join(path.file_name().ok_or_else(denied)?);
    let leaf = destination
        .file_name()
        .and_then(|name| name.to_str())
        .ok_or_else(denied)?;
    if leaf.len() > 180 {
        return Err(denied());
    }
    let lock_path = canonical.join(format!(".gs-bootstrap-{leaf}.lock"));
    let lock = std::fs::OpenOptions::new()
        .read(true)
        .write(true)
        .create(true)
        .truncate(false)
        .mode(0o600)
        .custom_flags(libc::O_CLOEXEC | libc::O_NOFOLLOW | libc::O_NONBLOCK)
        .open(&lock_path)
        .map_err(|_| denied())?;
    let lock_metadata = lock.metadata().map_err(|_| denied())?;
    if !lock_metadata.is_file()
        || lock_metadata.uid() != unsafe { libc::geteuid() }
        || lock_metadata.mode() & 0o7777 != 0o600
        || lock_metadata.nlink() != 1
        || lock_metadata.len() != 0
    {
        return Err(denied());
    }
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(2);
    loop {
        match fs2::FileExt::try_lock_exclusive(&lock) {
            Ok(()) => break,
            Err(error)
                if error.kind() == std::io::ErrorKind::WouldBlock
                    && std::time::Instant::now() < deadline =>
            {
                std::thread::sleep(std::time::Duration::from_millis(5))
            }
            Err(_) => return Err(denied()),
        }
    }
    let named = std::fs::symlink_metadata(lock_path).map_err(|_| denied())?;
    if named.dev() != lock_metadata.dev()
        || named.ino() != lock_metadata.ino()
        || named.nlink() != 1
    {
        return Err(denied());
    }
    // No overwrite, including racing creators. The existing path is still
    // checked by the normal no-follow/private-file read below.
    if destination.symlink_metadata().is_ok() {
        return Ok(lock);
    }
    let mut temporary = tempfile::NamedTempFile::new_in(&canonical).map_err(|_| denied())?;
    temporary
        .write_all(value.as_bytes())
        .map_err(|_| denied())?;
    temporary.as_file().sync_all().map_err(|_| denied())?;
    match temporary.persist_noclobber(&destination) {
        Ok(_) => (),
        Err(error) if error.error.kind() == std::io::ErrorKind::AlreadyExists => (),
        Err(_) => return Err(denied()),
    }
    std::fs::File::open(canonical)
        .and_then(|directory| directory.sync_all())
        .map_err(|_| denied())?;
    Ok(lock)
}

#[cfg(unix)]
fn private_stream() -> Result<std::os::unix::net::UnixStream> {
    use std::os::fd::{AsRawFd, FromRawFd};
    let mut address: libc::sockaddr_un = unsafe { std::mem::zeroed() };
    let mut length = std::mem::size_of_val(&address) as libc::socklen_t;
    if unsafe {
        libc::getsockname(
            3,
            (&mut address as *mut libc::sockaddr_un).cast(),
            &mut length,
        )
    } != 0
        || address.sun_family as i32 != libc::AF_UNIX
    {
        return Err(denied());
    }
    let mut kind: libc::c_int = 0;
    length = std::mem::size_of_val(&kind) as libc::socklen_t;
    if unsafe {
        libc::getsockopt(
            3,
            libc::SOL_SOCKET,
            libc::SO_TYPE,
            (&mut kind as *mut libc::c_int).cast(),
            &mut length,
        )
    } != 0
        || kind != libc::SOCK_STREAM
    {
        return Err(denied());
    }
    #[cfg(target_os = "linux")]
    {
        let mut peer: libc::ucred = unsafe { std::mem::zeroed() };
        length = std::mem::size_of_val(&peer) as libc::socklen_t;
        if unsafe {
            libc::getsockopt(
                3,
                libc::SOL_SOCKET,
                libc::SO_PEERCRED,
                (&mut peer as *mut libc::ucred).cast(),
                &mut length,
            )
        } != 0
            || peer.uid != unsafe { libc::geteuid() }
        {
            return Err(denied());
        }
    }
    #[cfg(target_os = "macos")]
    {
        let (mut uid, mut gid) = (0, 0);
        if unsafe { libc::getpeereid(3, &mut uid, &mut gid) } != 0
            || uid != unsafe { libc::geteuid() }
        {
            return Err(denied());
        }
    }
    let stream = unsafe { std::os::unix::net::UnixStream::from_raw_fd(3) };
    if unsafe { libc::fcntl(stream.as_raw_fd(), libc::F_SETFD, libc::FD_CLOEXEC) } < 0 {
        return Err(denied());
    }
    let timeout = Some(std::time::Duration::from_secs(5));
    stream.set_read_timeout(timeout).map_err(|_| denied())?;
    stream.set_write_timeout(timeout).map_err(|_| denied())?;
    Ok(stream)
}
#[cfg(unix)]
fn serve() -> Result<()> {
    let mut socket = private_stream()?;
    let mut header = [0; 4];
    socket.read_exact(&mut header).map_err(|_| denied())?;
    let size = u32::from_be_bytes(header) as usize;
    if size == 0 || size > REQUEST_LIMIT {
        return Err(denied());
    }
    let mut bytes = Zeroizing::new(vec![0; size]);
    socket.read_exact(&mut bytes).map_err(|_| denied())?;
    let mut extra = [0];
    if socket.read(&mut extra).map_err(|_| denied())? != 0 {
        return Err(denied());
    }
    let result =
        Request::decode(&bytes).and_then(|request| resolve(&request, &crate::credentials::Native));
    let (status, secret) = match result {
        Ok(Some(s)) => (0, Some(s)),
        Ok(None) => (1, None),
        Err(_) => (2, None),
    };
    let body = secret.as_ref().map_or(&[][..], |s| s.expose().as_bytes());
    socket
        .write_all(&[status])
        .and_then(|_| socket.write_all(&(body.len() as u32).to_be_bytes()))
        .and_then(|_| socket.write_all(body))
        .map_err(|_| denied())
}
/// Called before Clap/configuration. Private installed entry point, no diagnostics.
pub fn entry() -> Option<i32> {
    let mut args = std::env::args_os();
    args.next();
    if args.next().as_deref() != Some(std::ffi::OsStr::new(ENTRY)) {
        return None;
    }
    if args.next().is_some() {
        return Some(2);
    }
    #[cfg(unix)]
    {
        // A private broker is a short-lived process, including trickled requests.
        // The daemon also enforces its own shorter monotonic deadline and reaps it.
        let core = libc::rlimit {
            rlim_cur: 0,
            rlim_max: 0,
        };
        if unsafe { libc::setrlimit(libc::RLIMIT_CORE, &core) } != 0 {
            return Some(1);
        }
        unsafe {
            libc::signal(libc::SIGALRM, libc::SIG_DFL);
            libc::alarm(10);
        }
        Some(if serve().is_ok() { 0 } else { 1 })
    }
    #[cfg(not(unix))]
    {
        Some(1)
    }
}
