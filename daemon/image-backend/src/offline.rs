//! Offline import never constructs a network client. Index data is format input,
//! never authority for the digest selected by the registered caller plan.
use crate::{Outcome, Result, Status, acquire, plan::Plan, unpack};
use ocirender::image::LayerBlob;
use sha2::{Digest, Sha256};
use std::{
    ffi::CString,
    fs::{File, OpenOptions},
    io::{Read, Write},
    os::{
        fd::{AsRawFd, FromRawFd},
        unix::fs::{MetadataExt, OpenOptionsExt},
    },
    path::Path,
};

pub(crate) struct Source(pub(crate) File);
pub(crate) fn separate_roots(source: &Source, target: &Path) -> Result<()> {
    fn contains(ancestor: &std::fs::Metadata, start: &File) -> Result<bool> {
        let mut current = start.try_clone().map_err(|_| Status::Invalid)?;
        for _ in 0..256 {
            let info = current.metadata().map_err(|_| Status::Invalid)?;
            if (info.dev(), info.ino()) == (ancestor.dev(), ancestor.ino()) {
                return Ok(true);
            }
            let fd = unsafe {
                libc::openat(
                    current.as_raw_fd(),
                    c"..".as_ptr(),
                    libc::O_PATH | libc::O_DIRECTORY | libc::O_CLOEXEC | libc::O_NOFOLLOW,
                )
            };
            if fd < 0 {
                return Err(Status::Invalid);
            }
            let parent = unsafe { File::from_raw_fd(fd) };
            let above = parent.metadata().map_err(|_| Status::Invalid)?;
            if (info.dev(), info.ino()) == (above.dev(), above.ino()) {
                return Ok(false);
            }
            current = parent;
        }
        Err(Status::Invalid)
    }
    let target = File::open(target).map_err(|_| Status::Invalid)?;
    let source_info = source.0.metadata().map_err(|_| Status::Invalid)?;
    let target_info = target.metadata().map_err(|_| Status::Invalid)?;
    if contains(&source_info, &target)? || contains(&target_info, &source.0)? {
        return Err(Status::Invalid);
    }
    Ok(())
}
impl Source {
    pub(crate) fn new(fd: i32) -> Result<Self> {
        let copied = unsafe { libc::fcntl(fd, libc::F_DUPFD_CLOEXEC, 3) };
        if copied < 0 {
            return Err(Status::Invalid);
        }
        let file = unsafe { File::from_raw_fd(copied) };
        let meta = file.metadata().map_err(|_| Status::Invalid)?;
        if !meta.is_dir()
            || (meta.uid() != 0 && meta.uid() != unsafe { libc::geteuid() })
            || meta.mode() & 0o022 != 0
        {
            return Err(Status::Invalid);
        }
        Ok(Self(file))
    }
    pub(crate) fn open(&self, path: &str, maximum: u64) -> Result<File> {
        self.optional(path, maximum)?.ok_or(Status::Integrity)
    }
    pub(crate) fn optional(&self, path: &str, maximum: u64) -> Result<Option<File>> {
        let path = CString::new(path).map_err(|_| Status::Invalid)?;
        let mut how: libc::open_how = unsafe { std::mem::zeroed() };
        how.flags = (libc::O_RDONLY | libc::O_CLOEXEC | libc::O_NOFOLLOW | libc::O_NONBLOCK) as u64;
        how.resolve = libc::RESOLVE_BENEATH
            | libc::RESOLVE_NO_XDEV
            | libc::RESOLVE_NO_SYMLINKS
            | libc::RESOLVE_NO_MAGICLINKS;
        let fd = unsafe {
            libc::syscall(
                libc::SYS_openat2,
                self.0.as_raw_fd(),
                path.as_ptr(),
                &how,
                std::mem::size_of_val(&how),
            )
        } as i32;
        if fd < 0 {
            let error = std::io::Error::last_os_error().raw_os_error();
            if error == Some(libc::ENOENT) {
                return Ok(None);
            }
            if error == Some(libc::ENOSYS) || error == Some(libc::EINVAL) {
                return Err(Status::Unavailable);
            }
            return Err(Status::Integrity);
        }
        let file = unsafe { File::from_raw_fd(fd) };
        let meta = file.metadata().map_err(|_| Status::Integrity)?;
        if !meta.is_file()
            || meta.nlink() != 1
            || meta.len() > maximum
            || (meta.uid() != 0 && meta.uid() != unsafe { libc::geteuid() })
            || meta.mode() & 0o022 != 0
        {
            return Err(Status::Integrity);
        }
        Ok(Some(file))
    }
    pub(crate) fn small(&self, path: &str, maximum: u64) -> Result<Vec<u8>> {
        let mut bytes = Vec::new();
        self.open(path, maximum)?
            .take(maximum + 1)
            .read_to_end(&mut bytes)
            .map_err(|_| Status::Integrity)?;
        if bytes.len() as u64 > maximum {
            return Err(Status::Integrity);
        }
        Ok(bytes)
    }
}
fn copy_blob(
    source: &Source,
    d: &oci_client::manifest::OciDescriptor,
    name: &str,
    path: &Path,
    remaining: &mut u64,
) -> Result<()> {
    let size = acquire::descriptor(d)?;
    if size > *remaining {
        return Err(Status::Quota);
    }
    let mut input = source.open(name, size)?;
    if input.metadata().map_err(|_| Status::Integrity)?.len() != size {
        return Err(Status::Integrity);
    }
    let mut output = OpenOptions::new()
        .write(true)
        .create_new(true)
        .mode(0o600)
        .open(path)
        .map_err(|_| Status::Transfer)?;
    let mut hash = Sha256::new();
    let mut count = 0u64;
    let mut buffer = [0u8; 32768];
    loop {
        let read = input.read(&mut buffer).map_err(|_| Status::Integrity)?;
        if read == 0 {
            break;
        }
        count = count.checked_add(read as u64).ok_or(Status::Quota)?;
        if count > size {
            return Err(Status::Integrity);
        }
        hash.update(&buffer[..read]);
        output
            .write_all(&buffer[..read])
            .map_err(|_| Status::Transfer)?;
    }
    if count != size || format!("sha256:{}", hex::encode(hash.finalize())) != d.digest {
        return Err(Status::Integrity);
    }
    output.sync_all().map_err(|_| Status::Transfer)?;
    *remaining -= count;
    Ok(())
}
pub(crate) fn stage(plan: &Plan, source_fd: i32, root: &Path) -> Result<Outcome> {
    materialize(plan, source_fd, root, false)
}
pub(crate) fn stage_cache(plan: &Plan, source_fd: i32, root: &Path) -> Result<Outcome> {
    materialize(plan, source_fd, root, true)
}
fn materialize(plan: &Plan, source_fd: i32, root: &Path, cached: bool) -> Result<Outcome> {
    let reference = plan.validate()?;
    let source = Source::new(source_fd)?;
    separate_roots(&source, root)?;
    let raw = if cached {
        let info = source.0.metadata().map_err(|_| Status::Invalid)?;
        let target = root.metadata().map_err(|_| Status::Invalid)?;
        if info.uid() != unsafe { libc::geteuid() }
            || info.mode() & 0o7777 != 0o700
            || (info.dev() == target.dev() && info.ino() == target.ino())
        {
            return Err(Status::Invalid);
        }
        source.small("manifest.json", 1024 * 1024)?
    } else {
        let marker = source.small("oci-layout", 4096)?;
        let layout: oci_spec::image::OciLayout =
            serde_json::from_slice(&marker).map_err(|_| Status::Integrity)?;
        if layout.image_layout_version() != "1.0.0" {
            return Err(Status::Invalid);
        }
        let index = source.small("index.json", 1024 * 1024)?;
        let index: oci_spec::image::ImageIndex =
            serde_json::from_slice(&index).map_err(|_| Status::Integrity)?;
        if index.schema_version() != 2 || index.manifests().len() > 128 {
            return Err(Status::Integrity);
        }
        // Select by the caller's trusted pin, never mutable bundle tags/annotations.
        let digest = reference.digest().ok_or(Status::Invalid)?;
        source.small(&format!("blobs/sha256/{}", &digest[7..]), 1024 * 1024)?
    };
    let manifest = acquire::verified_manifest(plan, &raw)?;
    std::fs::create_dir(root.join("blobs")).map_err(|_| Status::Transfer)?;
    acquire::write_new(&root.join("manifest.json"), &raw)?;
    let mut remaining = plan.maximum_download_bytes;
    copy_blob(
        &source,
        &manifest.config,
        &if cached {
            "config.json".to_owned()
        } else {
            format!("blobs/sha256/{}", &manifest.config.digest[7..])
        },
        &root.join("config.json"),
        &mut remaining,
    )?;
    let config = std::fs::read(root.join("config.json")).map_err(|_| Status::Integrity)?;
    acquire::verified_config(plan, &config)?;
    let mut layers = Vec::new();
    for (index, descriptor) in manifest.layers.iter().enumerate() {
        let path = root.join("blobs").join(format!("layer-{index}"));
        let name = if cached {
            format!("blobs/layer-{index}")
        } else {
            format!("blobs/sha256/{}", &descriptor.digest[7..])
        };
        copy_blob(&source, descriptor, &name, &path, &mut remaining)?;
        layers.push(LayerBlob {
            path,
            media_type: descriptor.media_type.clone(),
            index,
        });
    }
    let merged_bytes = unpack::unpack(layers, root, plan.maximum_merged_bytes)?;
    Ok(Outcome {
        downloaded_bytes: plan.maximum_download_bytes - remaining,
        merged_bytes,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{Status, plan::Plan};
    use sha2::{Digest, Sha256};
    use std::{fs, os::fd::AsRawFd};
    fn digest(bytes: &[u8]) -> String {
        format!("sha256:{}", hex::encode(Sha256::digest(bytes)))
    }
    fn fixture(root: &std::path::Path) -> Plan {
        fs::create_dir_all(root.join("blobs/sha256")).unwrap();
        let mut layer = Vec::new();
        {
            let mut archive = tar::Builder::new(&mut layer);
            let mut header = tar::Header::new_ustar();
            header.set_mode(0o644);
            header.set_size(8);
            header.set_cksum();
            archive
                .append_data(&mut header, "app/data", &b"verified"[..])
                .unwrap();
            archive.finish().unwrap();
        }
        let config =
            serde_json::to_vec(&serde_json::json!({"os":"linux","architecture":"arm64"})).unwrap();
        let manifest=serde_json::to_vec(&serde_json::json!({"schemaVersion":2,"mediaType":"application/vnd.oci.image.manifest.v1+json",
            "config":{"mediaType":"application/vnd.oci.image.config.v1+json","digest":digest(&config),"size":config.len()},
            "layers":[{"mediaType":"application/vnd.oci.image.layer.v1.tar","digest":digest(&layer),"size":layer.len()}]})).unwrap();
        for bytes in [&config, &layer, &manifest] {
            fs::write(root.join("blobs/sha256").join(&digest(bytes)[7..]), bytes).unwrap();
        }
        fs::write(
            root.join("oci-layout"),
            br#"{"imageLayoutVersion":"1.0.0"}"#,
        )
        .unwrap();
        fs::write(root.join("index.json"),serde_json::to_vec(&serde_json::json!({"schemaVersion":2,"manifests":[{
            "mediaType":"application/vnd.oci.image.manifest.v1+json","digest":digest(&manifest),"size":manifest.len()}]})).unwrap()).unwrap();
        Plan {
            schema_version: 1,
            reference: format!("ghcr.io/fixture/image@{}", digest(&manifest)),
            config_digest: digest(&config),
            os: "linux".into(),
            architecture: "arm64".into(),
            token_origins: vec![],
            blob_origins: vec![],
            maximum_download_bytes: 65536,
            maximum_merged_bytes: 65536,
            timeout_ms: 1000,
        }
    }
    #[test]
    fn import_verifies_exact_pinned_bytes_without_network_or_index_authority() {
        let input = tempfile::tempdir().unwrap();
        let plan = fixture(input.path());
        let source = fs::File::open(input.path()).unwrap();
        let output = tempfile::tempdir().unwrap();
        let result = stage(&plan, source.as_raw_fd(), output.path()).unwrap();
        assert!(result.downloaded_bytes > 0 && result.merged_bytes > 0);
        assert_eq!(
            fs::read(output.path().join("rootfs/app/data")).unwrap(),
            b"verified"
        );
        assert!(source.metadata().unwrap().is_dir());
        // An index cannot nominate another digest instead of the registered pin.
        let mut wrong = plan;
        wrong.reference = format!("ghcr.io/fixture/image@sha256:{}", "a".repeat(64));
        let output = tempfile::tempdir().unwrap();
        assert!(stage(&wrong, source.as_raw_fd(), output.path()).is_err());
        assert!(fs::read_dir(output.path()).unwrap().next().is_none());
    }
    #[test]
    fn offline_quota_platform_and_symlink_fail_without_activation() {
        let input = tempfile::tempdir().unwrap();
        let mut plan = fixture(input.path());
        let source = fs::File::open(input.path()).unwrap();
        let output = tempfile::tempdir().unwrap();
        plan.maximum_download_bytes = 1;
        assert_eq!(
            stage(&plan, source.as_raw_fd(), output.path()).err(),
            Some(Status::Quota)
        );
        assert!(fs::read_dir(output.path()).unwrap().next().is_none());
        plan.maximum_download_bytes = 65536;
        plan.architecture = "amd64".into();
        assert_eq!(
            stage(&plan, source.as_raw_fd(), output.path()).err(),
            Some(Status::Integrity)
        );
        assert!(!output.path().join("rootfs").exists());
        let moved = input.path().join("held");
        fs::rename(input.path().join("blobs"), &moved).unwrap();
        std::os::unix::fs::symlink(&moved, input.path().join("blobs")).unwrap();
        let output = tempfile::tempdir().unwrap();
        assert!(stage(&plan, source.as_raw_fd(), output.path()).is_err());
        assert!(fs::read_dir(output.path()).unwrap().next().is_none());
    }
    #[test]
    fn offline_corrupt_layer_cannot_become_a_rootfs() {
        let input = tempfile::tempdir().unwrap();
        let plan = fixture(input.path());
        let source = fs::File::open(input.path()).unwrap();
        let manifest = fs::read(
            input
                .path()
                .join("blobs/sha256")
                .join(plan.reference.rsplit_once("sha256:").unwrap().1),
        )
        .unwrap();
        let manifest: oci_client::manifest::OciImageManifest =
            serde_json::from_slice(&manifest).unwrap();
        let path = input
            .path()
            .join("blobs/sha256")
            .join(&manifest.layers[0].digest[7..]);
        let mut bytes = fs::read(&path).unwrap();
        bytes[512] ^= 1;
        fs::write(path, bytes).unwrap();
        let output = tempfile::tempdir().unwrap();
        assert_eq!(
            stage(&plan, source.as_raw_fd(), output.path()).err(),
            Some(Status::Integrity)
        );
        assert!(!output.path().join("rootfs").exists());
    }
    #[test]
    fn workload_materialization_uses_verified_blobs_and_never_shared_rootfs_or_http() {
        use std::os::unix::fs::PermissionsExt;
        let input = tempfile::tempdir().unwrap();
        let plan = fixture(input.path());
        let input_fd = fs::File::open(input.path()).unwrap();
        let cache = tempfile::tempdir().unwrap();
        fs::set_permissions(cache.path(), fs::Permissions::from_mode(0o700)).unwrap();
        stage(&plan, input_fd.as_raw_fd(), cache.path()).unwrap();
        let cached = fs::File::open(cache.path()).unwrap();
        let original = cache.path().join("rootfs/app/data");
        fs::write(&original, b"never trust an extracted tree as image input").unwrap();
        let work = tempfile::tempdir().unwrap();
        stage_cache(&plan, cached.as_raw_fd(), work.path()).unwrap();
        let clone = work.path().join("rootfs/app/data");
        assert_eq!(fs::read(&clone).unwrap(), b"verified");
        let a = original.metadata().unwrap();
        let b = clone.metadata().unwrap();
        assert_ne!((a.dev(), a.ino()), (b.dev(), b.ino()));
        fs::write(&clone, b"owned workload mutation").unwrap();
        assert_eq!(
            fs::read(original).unwrap(),
            b"never trust an extracted tree as image input"
        );
        fs::remove_file(cache.path().join("blobs/layer-0")).unwrap();
        let missing = tempfile::tempdir().unwrap();
        assert_eq!(
            stage_cache(&plan, cached.as_raw_fd(), missing.path()).err(),
            Some(Status::Integrity)
        );
        assert!(!missing.path().join("rootfs").exists());
        assert!(cached.metadata().unwrap().is_dir());
        let nested = cache.path().join("nested-target");
        fs::create_dir(&nested).unwrap();
        assert_eq!(
            stage_cache(&plan, cached.as_raw_fd(), &nested).err(),
            Some(Status::Invalid)
        );
        assert!(fs::read_dir(nested).unwrap().next().is_none());
    }
}
