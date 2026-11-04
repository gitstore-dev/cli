//! Resume at verified blob boundaries. Incomplete bytes are never appended: in
//! the absence of a strong HTTP range validator the library restarts that blob.
use crate::{Result, Status, acquire, offline::Source, plan::Plan};
use sha2::{Digest, Sha256};
use std::{
    fs::OpenOptions,
    io::{Read, Seek, SeekFrom, Write},
    os::unix::fs::{MetadataExt, OpenOptionsExt},
    path::Path,
};

pub(crate) struct CachedBlobs(Source, Vec<u8>);
impl CachedBlobs {
    pub(crate) fn new(plan: &Plan, fd: i32, destination: &Path) -> Result<Self> {
        let source = Source::new(fd)?;
        crate::offline::separate_roots(&source, destination)?;
        let source_meta = source.0.metadata().map_err(|_| Status::Invalid)?;
        let target_meta = destination.metadata().map_err(|_| Status::Invalid)?;
        if source_meta.uid() != unsafe { libc::geteuid() }
            || source_meta.mode() & 0o7777 != 0o700
            || (source_meta.dev() == target_meta.dev() && source_meta.ino() == target_meta.ino())
        {
            return Err(Status::Invalid);
        }
        // A changed immutable manifest is refused before constructing an HTTP
        // client. The owner must separately match persisted plan/consent/source.
        let raw = source.small("manifest.json", 1024 * 1024)?;
        acquire::verified_manifest(plan, &raw)?;
        Ok(Self(source, raw))
    }
    pub(crate) fn manifest(&self) -> &[u8] {
        &self.1
    }
    pub(crate) fn copy(
        &self,
        descriptor: &oci_client::manifest::OciDescriptor,
        name: &str,
        destination: &Path,
        remaining: &mut u64,
    ) -> Result<bool> {
        let expected = acquire::descriptor(descriptor)?;
        if expected > *remaining {
            return Err(Status::Quota);
        }
        let Some(mut input) = self.0.optional(name, expected)? else {
            return Ok(false);
        };
        if input.metadata().map_err(|_| Status::Integrity)?.len() < expected {
            return Ok(false);
        }
        let mut hash = Sha256::new();
        let mut count = 0u64;
        let mut buffer = [0u8; 32768];
        loop {
            let size = input.read(&mut buffer).map_err(|_| Status::Integrity)?;
            if size == 0 {
                break;
            }
            count = count.checked_add(size as u64).ok_or(Status::Quota)?;
            if count > expected {
                return Err(Status::Integrity);
            }
            hash.update(&buffer[..size]);
        }
        if count != expected
            || format!("sha256:{}", hex::encode(hash.finalize())) != descriptor.digest
        {
            return Err(Status::Integrity);
        }
        input
            .seek(SeekFrom::Start(0))
            .map_err(|_| Status::Integrity)?;
        let mut output = OpenOptions::new()
            .write(true)
            .create_new(true)
            .mode(0o600)
            .open(destination)
            .map_err(|_| Status::Transfer)?;
        // Recheck the bytes copied, so a concurrently changed source cannot
        // activate after a successful earlier hash. Failure leaves only staging.
        hash = Sha256::new();
        count = 0;
        loop {
            let size = input.read(&mut buffer).map_err(|_| Status::Integrity)?;
            if size == 0 {
                break;
            }
            count = count.checked_add(size as u64).ok_or(Status::Quota)?;
            if count > expected {
                return Err(Status::Integrity);
            }
            hash.update(&buffer[..size]);
            output
                .write_all(&buffer[..size])
                .map_err(|_| Status::Transfer)?;
        }
        if count != expected
            || format!("sha256:{}", hex::encode(hash.finalize())) != descriptor.digest
        {
            return Err(Status::Integrity);
        }
        output.sync_all().map_err(|_| Status::Transfer)?;
        *remaining -= count;
        Ok(true)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::{fs, os::fd::AsRawFd};
    #[test]
    fn complete_blobs_are_rehashed_partial_blobs_restart_and_changed_plan_is_refused() {
        use std::os::unix::fs::PermissionsExt;
        let source = tempfile::tempdir().unwrap();
        fs::set_permissions(source.path(), fs::Permissions::from_mode(0o700)).unwrap();
        let target = tempfile::tempdir().unwrap();
        let bytes = b"verified";
        let descriptor = oci_client::manifest::OciDescriptor {
            media_type: "application/vnd.oci.image.layer.v1.tar".into(),
            digest: acquire::sha256(bytes),
            size: 8,
            ..Default::default()
        };
        let raw = serde_json::to_vec(&serde_json::json!({"schemaVersion":2,
            "mediaType":"application/vnd.oci.image.manifest.v1+json",
            "config":{"mediaType":"application/vnd.oci.image.config.v1+json",
                      "digest":acquire::sha256(b"{}"),"size":2},"layers":[descriptor]}))
        .unwrap();
        fs::write(source.path().join("manifest.json"), &raw).unwrap();
        fs::create_dir(source.path().join("blobs")).unwrap();
        fs::write(source.path().join("blobs/layer-0"), bytes).unwrap();
        let plan = Plan {
            schema_version: 1,
            reference: format!("registry.example/owned/image@{}", acquire::sha256(&raw)),
            config_digest: acquire::sha256(b"{}"),
            os: "linux".into(),
            architecture: "arm64".into(),
            token_origins: vec![],
            blob_origins: vec![],
            maximum_download_bytes: 1024,
            maximum_merged_bytes: 4096,
            timeout_ms: 1000,
        };
        let fd = fs::File::open(source.path()).unwrap();
        let cached = CachedBlobs::new(&plan, fd.as_raw_fd(), target.path()).unwrap();
        let output = target.path().join("copied");
        let mut remaining = 8;
        assert!(
            cached
                .copy(&descriptor, "blobs/layer-0", &output, &mut remaining)
                .unwrap()
        );
        assert_eq!(remaining, 0);
        assert_eq!(fs::read(&output).unwrap(), bytes);
        fs::remove_file(&output).unwrap();
        fs::write(source.path().join("blobs/layer-0"), b"partial").unwrap();
        remaining = 8;
        assert!(
            !cached
                .copy(&descriptor, "blobs/layer-0", &output, &mut remaining)
                .unwrap()
        );
        assert_eq!(remaining, 8);
        assert!(!output.exists());
        fs::write(source.path().join("blobs/layer-0"), b"tampered").unwrap();
        assert_eq!(
            cached
                .copy(&descriptor, "blobs/layer-0", &output, &mut remaining)
                .err(),
            Some(Status::Integrity)
        );
        assert!(!output.exists());
        let mut changed = plan;
        changed.reference = format!("registry.example/owned/image@sha256:{}", "a".repeat(64));
        assert!(matches!(
            CachedBlobs::new(&changed, fd.as_raw_fd(), target.path()),
            Err(Status::Integrity)
        ));
        assert!(matches!(
            CachedBlobs::new(&changed, fd.as_raw_fd(), source.path()),
            Err(Status::Invalid)
        ));
        assert!(fd.metadata().unwrap().is_dir());
    }
}
