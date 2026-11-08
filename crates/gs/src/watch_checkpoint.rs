//! Private scope-bound watch positions. Server cursors alone have no selection binding.
use crate::{
    error::{Error, Result},
    input,
};
use fs2::FileExt;
use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::{
    fs::{self, File, OpenOptions},
    hash::{DefaultHasher, Hash, Hasher},
    io::Write,
    os::unix::fs::{MetadataExt, OpenOptionsExt},
    path::{Path, PathBuf},
};

#[derive(Clone, PartialEq, Deserialize, Serialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
pub struct Scope {
    pub application_version: String,
    pub profile: String,
    pub identity: String,
    pub kind: String,
    pub namespace: Option<String>,
    pub name: Option<String>,
    pub selector: Value,
}
#[derive(Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
pub struct Checkpoint {
    pub schema_version: u32,
    pub scope: Scope,
    pub resource_version: String,
    pub last_emitted_event: Option<Value>,
    pub connection_state: String,
    pub retries: u32,
}
pub struct Store {
    path: PathBuf,
    scope: Scope,
    _lock: File,
}
impl Store {
    pub fn prepare(scope: Scope, selected: Option<&Path>) -> Result<Self> {
        let encoded = serde_json::to_string(&scope)
            .map_err(|_| Error::usage("Cannot encode watch selection"))?;
        if encoded.len() > 32768 {
            return Err(Error::usage("Watch selection exceeds 32 KiB"));
        }
        let path = if let Some(path) = selected {
            path.to_owned()
        } else {
            let base = std::env::var_os("XDG_STATE_HOME")
                .map(PathBuf::from)
                .or_else(|| {
                    std::env::var_os("HOME").map(|home| PathBuf::from(home).join(".local/state"))
                })
                .ok_or_else(|| {
                    Error::usage("Select --checkpoint-file or a user state directory")
                })?;
            // A filename hash is only an index. Exact stored scope equality is
            // mandatory, so collisions can never authorize cursor reuse.
            let mut index = DefaultHasher::new();
            encoded.hash(&mut index);
            base.join("gitstore/watch")
                .join(format!("{:016x}.json", index.finish()))
        };
        if !path.is_absolute() {
            return Err(Error::usage("Watch checkpoint path must be absolute"));
        }
        let parent = path
            .parent()
            .ok_or_else(|| Error::usage("Select a checkpoint filename"))?;
        // Resolve an existing ancestor first: macOS /var and /tmp are platform
        // aliases. All created components and the final parent still undergo
        // the same owner/mode checks on the canonical ordinary directory.
        let mut ancestor = parent;
        let mut missing = Vec::new();
        while !ancestor.exists() {
            missing.push(
                ancestor
                    .file_name()
                    .ok_or_else(|| Error::usage("Invalid checkpoint parent"))?
                    .to_owned(),
            );
            ancestor = ancestor
                .parent()
                .ok_or_else(|| Error::usage("Invalid checkpoint parent"))?;
        }
        let mut parent = ancestor
            .canonicalize()
            .map_err(|_| Error::usage("Cannot resolve checkpoint parent"))?;
        for component in missing.into_iter().rev() {
            parent.push(component);
        }
        crate::daemon_start::directory(&parent).map_err(|_| {
            Error::usage("Checkpoint parent must be a trusted owned mode-0700 directory")
        })?;
        let path = parent.join(
            path.file_name()
                .ok_or_else(|| Error::usage("Select a checkpoint filename"))?,
        );
        let mut lock_name = path.as_os_str().to_owned();
        lock_name.push(".lock");
        let lock = OpenOptions::new()
            .read(true)
            .write(true)
            .create(true)
            .truncate(false)
            .mode(0o600)
            .custom_flags(libc::O_NOFOLLOW | libc::O_NONBLOCK)
            .open(PathBuf::from(lock_name))
            .map_err(|_| Error::usage("Cannot open private checkpoint lock"))?;
        let info = lock
            .metadata()
            .map_err(|_| Error::usage("Cannot inspect checkpoint lock"))?;
        if !info.is_file()
            || info.uid() != unsafe { libc::geteuid() }
            || info.mode() & 0o077 != 0
            || info.nlink() != 1
        {
            return Err(Error::usage("Unsafe checkpoint lock identity"));
        }
        lock.try_lock_exclusive().map_err(|_| {
            Error::new(
                5,
                "CONFLICT",
                "Checkpoint is in use; concurrent observers need distinct checkpoint files",
            )
        })?;
        let store = Self {
            path,
            scope,
            _lock: lock,
        };
        store.load()?;
        Ok(store)
    }
    pub fn load(&self) -> Result<Option<Checkpoint>> {
        let metadata = match fs::symlink_metadata(&self.path) {
            Ok(metadata) => metadata,
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Ok(None),
            Err(_) => return Err(Error::usage("Cannot inspect watch checkpoint")),
        };
        if !metadata.is_file() || metadata.nlink() != 1 || metadata.len() > 262144 {
            return Err(Error::usage(
                "Checkpoint must be a bounded ordinary single-link file",
            ));
        }
        let value = input::json(&input::protected(&self.path)?)?;
        let record: Checkpoint = serde_json::from_value(value)
            .map_err(|_| Error::usage("Invalid watch checkpoint fields"))?;
        if record.schema_version != 1
            || record.scope != self.scope
            || record.resource_version.is_empty()
            || record.resource_version.len() > 65536
            || !matches!(
                record.connection_state.as_str(),
                "streaming" | "reconnecting" | "expired" | "failed" | "stopped"
            )
        {
            return Err(Error::usage(
                "Checkpoint cannot cross application, installation, identity, profile or selection",
            ));
        }
        Ok(Some(record))
    }
    pub fn save(&self, record: &Checkpoint) -> Result<()> {
        if record.scope != self.scope {
            return Err(Error::usage("Checkpoint scope changed"));
        }
        self.load()?;
        let bytes =
            serde_json::to_vec(record).map_err(|_| Error::remote("Cannot encode checkpoint"))?;
        if bytes.len() > 262144 {
            return Err(Error::remote("Checkpoint exceeds 256 KiB"));
        }
        let parent = self
            .path
            .parent()
            .ok_or_else(|| Error::usage("Missing checkpoint parent"))?;
        let directory =
            File::open(parent).map_err(|_| Error::remote("Cannot pin checkpoint parent"))?;
        let mut temporary = tempfile::NamedTempFile::new_in(parent)
            .map_err(|_| Error::remote("Cannot prepare checkpoint"))?;
        temporary
            .write_all(&bytes)
            .and_then(|_| temporary.write_all(b"\n"))
            .and_then(|_| temporary.as_file().sync_all())
            .map_err(|_| Error::remote("Cannot persist checkpoint"))?;
        temporary
            .persist(&self.path)
            .map_err(|_| Error::remote("Cannot atomically replace checkpoint"))?;
        directory
            .sync_all()
            .map_err(|_| Error::remote("Checkpoint changed but crash durability is unconfirmed"))
    }
    pub fn scope(&self) -> &Scope {
        &self.scope
    }
}
