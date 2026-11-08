//! Explicit atomic session persistence. Runtime add-only credentials are separate.
use crate::{
    credentials::{Native, Secret, Store},
    error::{Error, Result},
    input,
};
use fs2::FileExt;
use std::{
    fs::{self, File, OpenOptions},
    hash::{DefaultHasher, Hash, Hasher},
    io::Write,
    os::unix::fs::{MetadataExt, OpenOptionsExt},
    path::{Path, PathBuf},
};

type Identity = (u64, u64, u64, i64, i64, i64, i64);
fn native_lock(key: &str, base: &Path) -> Result<File> {
    if !base.is_absolute() {
        return Err(Error::usage(
            "Native session state directory must be absolute",
        ));
    }
    let selected = base.join("gitstore/auth");
    let mut ancestor = selected.as_path();
    let mut missing = Vec::new();
    while !ancestor.exists() {
        missing.push(
            ancestor
                .file_name()
                .ok_or_else(|| Error::auth("Invalid session state directory"))?
                .to_owned(),
        );
        ancestor = ancestor
            .parent()
            .ok_or_else(|| Error::auth("Invalid session state directory"))?;
    }
    let mut parent = ancestor
        .canonicalize()
        .map_err(|_| Error::auth("Cannot resolve session state directory"))?;
    for name in missing.into_iter().rev() {
        parent.push(name);
    }
    crate::daemon_start::directory(&parent).map_err(|_| {
        Error::auth("Native session locks require a trusted private user state directory")
    })?;
    // Hashing indexes the lock only, never credentials. A collision can refuse
    // concurrent work but cannot select a different credential-store identity.
    let mut index = DefaultHasher::new();
    key.hash(&mut index);
    let lock = OpenOptions::new()
        .read(true)
        .write(true)
        .create(true)
        .truncate(false)
        .mode(0o600)
        .custom_flags(libc::O_NOFOLLOW | libc::O_NONBLOCK)
        .open(parent.join(format!("{:016x}.lock", index.finish())))
        .map_err(|_| Error::auth("Cannot open native session lock"))?;
    let info = lock
        .metadata()
        .map_err(|_| Error::auth("Cannot inspect native session lock"))?;
    if !info.is_file()
        || info.uid() != unsafe { libc::geteuid() }
        || info.mode() & 0o077 != 0
        || info.nlink() != 1
    {
        return Err(Error::auth("Unsafe native session lock identity"));
    }
    lock.try_lock_exclusive().map_err(|_| {
        Error::new(
            5,
            "CONFLICT",
            "Authentication for this exact credential identity is already in progress",
        )
    })?;
    Ok(lock)
}
fn identity(path: &Path) -> Result<Option<Identity>> {
    let metadata = match fs::symlink_metadata(path) {
        Ok(metadata) => metadata,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Ok(None),
        Err(_) => return Err(Error::auth("Cannot inspect session destination")),
    };
    if !metadata.is_file()
        || metadata.uid() != unsafe { libc::geteuid() }
        || metadata.mode() & 0o077 != 0
        || metadata.nlink() != 1
    {
        return Err(Error::auth(
            "Session destination must be an ordinary single-link owned private file",
        ));
    }
    Ok(Some((
        metadata.dev(),
        metadata.ino(),
        metadata.len(),
        metadata.mtime(),
        metadata.mtime_nsec(),
        metadata.ctime(),
        metadata.ctime_nsec(),
    )))
}
pub enum Storage {
    Native {
        key: String,
        previous: Option<Secret>,
        _lock: File,
    },
    File {
        path: PathBuf,
        expected: Option<Identity>,
        _lock: File,
    },
}
impl Storage {
    pub fn prepare(path: Option<&Path>, key: String) -> Result<Self> {
        let Some(path) = path else {
            let base = std::env::var_os("XDG_STATE_HOME")
                .map(PathBuf::from)
                .or_else(|| {
                    std::env::var_os("HOME").map(|home| PathBuf::from(home).join(".local/state"))
                })
                .ok_or_else(|| {
                    Error::auth("Select a private user state directory for native session locking")
                })?;
            let lock = native_lock(&key, &base)?;
            let previous = Native.read(&key)?;
            return Ok(Self::Native {
                key,
                previous,
                _lock: lock,
            });
        };
        if !path.is_absolute() {
            return Err(Error::usage(
                "Session persistence requires an absolute protected file path",
            ));
        }
        let parent = path
            .parent()
            .and_then(|parent| parent.canonicalize().ok())
            .ok_or_else(|| Error::auth("Select an existing private session parent"))?;
        let info = parent
            .metadata()
            .map_err(|_| Error::auth("Cannot inspect session parent"))?;
        if info.uid() != unsafe { libc::geteuid() } || info.mode() & 0o022 != 0 {
            return Err(Error::auth(
                "Session parent must be owned and not writable by other users",
            ));
        }
        let path = parent.join(
            path.file_name()
                .ok_or_else(|| Error::usage("Select a session filename"))?,
        );
        let mut lock_name = path.as_os_str().to_owned();
        lock_name.push(".gs-auth.lock");
        let lock = OpenOptions::new()
            .read(true)
            .write(true)
            .create(true)
            .truncate(false)
            .mode(0o600)
            .custom_flags(libc::O_NOFOLLOW | libc::O_NONBLOCK)
            .open(PathBuf::from(lock_name))
            .map_err(|_| Error::auth("Cannot open private session lock"))?;
        let metadata = lock
            .metadata()
            .map_err(|_| Error::auth("Cannot inspect session lock"))?;
        if !metadata.is_file()
            || metadata.uid() != unsafe { libc::geteuid() }
            || metadata.mode() & 0o077 != 0
            || metadata.nlink() != 1
        {
            return Err(Error::auth("Unsafe session lock identity"));
        }
        lock.try_lock_exclusive().map_err(|_| {
            Error::new(
                5,
                "CONFLICT",
                "Session persistence is busy; wait for the current authentication command",
            )
        })?;
        let expected = identity(&path)?;
        Ok(Self::File {
            path,
            expected,
            _lock: lock,
        })
    }
    pub fn read(&self) -> Result<Secret> {
        match self {
            Self::File { path, .. } => Secret::new(input::protected(path)?),
            Self::Native { previous, .. } => previous
                .as_ref()
                .ok_or_else(|| {
                    Error::auth("No session for the exact installation/issuer/account/provider")
                })
                .and_then(|value| Secret::new(value.expose().to_owned())),
        }
    }
    fn unchanged(&self) -> Result<()> {
        match self {
            Self::File { path, expected, .. } if identity(path)? != *expected => {
                Err(Error::auth("Session file changed concurrently"))
            }
            Self::Native { key, previous, .. } => {
                let current = Native.read(key)?;
                if current.as_ref().map(Secret::expose) != previous.as_ref().map(Secret::expose) {
                    return Err(Error::auth("OS session changed concurrently"));
                }
                Ok(())
            }
            _ => Ok(()),
        }
    }
    pub fn write(&self, value: &Secret) -> Result<()> {
        self.unchanged()?;
        match self {
            Self::Native { key, .. } => Native.write_session(key, value),
            Self::File { path, .. } => {
                let parent = path
                    .parent()
                    .ok_or_else(|| Error::auth("Missing session parent"))?;
                let directory =
                    File::open(parent).map_err(|_| Error::auth("Cannot pin session parent"))?;
                let mut temporary = tempfile::NamedTempFile::new_in(parent)
                    .map_err(|_| Error::auth("Cannot prepare session update"))?;
                temporary
                    .write_all(value.expose().as_bytes())
                    .and_then(|_| temporary.write_all(b"\n"))
                    .and_then(|_| temporary.as_file().sync_all())
                    .map_err(|_| Error::auth("Cannot persist session bytes"))?;
                self.unchanged()?;
                temporary
                    .persist(path)
                    .map_err(|_| Error::auth("Cannot atomically replace session file"))?;
                directory
                    .sync_all()
                    .map_err(|_| Error::auth("Session changed but crash durability is unconfirmed"))
            }
        }
    }
    pub fn remove(&self) -> Result<bool> {
        self.unchanged()?;
        match self {
            Self::Native { key, .. } => Native.remove(key),
            Self::File { path, expected, .. } => {
                if expected.is_none() {
                    return Ok(false);
                }
                let directory = File::open(
                    path.parent()
                        .ok_or_else(|| Error::auth("Missing session parent"))?,
                )
                .map_err(|_| Error::auth("Cannot pin session parent"))?;
                fs::remove_file(path)
                    .map_err(|_| Error::auth("Cannot remove revoked session file"))?;
                directory.sync_all().map_err(|_| {
                    Error::auth("Session removed but crash durability is unconfirmed")
                })?;
                Ok(true)
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn native_session_locks_serialize_only_the_selected_identity_and_reject_exposure() {
        let root = tempfile::tempdir().unwrap();
        let first = native_lock("synthetic identity one", root.path()).unwrap();
        assert_eq!(
            native_lock("synthetic identity one", root.path())
                .unwrap_err()
                .exit,
            5
        );
        let independent = native_lock("synthetic identity two", root.path()).unwrap();
        drop(first);
        let first = native_lock("synthetic identity one", root.path()).unwrap();
        assert_eq!(first.metadata().unwrap().mode() & 0o777, 0o600);
        drop(first);
        drop(independent);
        for entry in fs::read_dir(root.path().join("gitstore/auth")).unwrap() {
            fs::set_permissions(
                entry.unwrap().path(),
                std::os::unix::fs::PermissionsExt::from_mode(0o644),
            )
            .unwrap();
        }
        assert!(native_lock("synthetic identity one", root.path()).is_err());
    }
}
