use crate::{Result, Status};
use ocirender::image::LayerBlob;
use std::{
    fs::{File, OpenOptions},
    io::{self, Write},
    path::Path,
};

struct BoundedWriter {
    file: File,
    bytes: u64,
    limit: u64,
    exhausted: bool,
}
impl Write for BoundedWriter {
    fn write(&mut self, bytes: &[u8]) -> io::Result<usize> {
        if bytes.len() as u64 > self.limit - self.bytes {
            self.exhausted = true;
            return Err(io::Error::other("Image merge quota"));
        }
        let size = self.file.write(bytes)?;
        self.bytes += size as u64;
        Ok(size)
    }
    fn flush(&mut self) -> io::Result<()> {
        self.file.flush()
    }
}
pub(crate) fn unpack(layers: Vec<LayerBlob>, root: &Path, limit: u64) -> Result<u64> {
    let file = OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(root.join("merged.tar"))
        .map_err(|_| Status::Unpack)?;
    let mut output = BoundedWriter {
        file,
        bytes: 0,
        limit,
        exhausted: false,
    };
    if ocirender::overlay::merge_layers_into(layers, &mut output).is_err() {
        return Err(if output.exhausted {
            Status::Quota
        } else {
            Status::Unpack
        });
    }
    output.file.sync_all().map_err(|_| Status::Unpack)?;
    let file = File::open(root.join("merged.tar")).map_err(|_| Status::Unpack)?;
    let mut archive = tar::Archive::new(file);
    archive.set_preserve_permissions(false);
    archive.set_preserve_ownerships(false);
    archive.set_unpack_xattrs(false);
    archive.set_overwrite(false);
    archive
        .unpack(root.join("rootfs"))
        .map_err(|_| Status::Unpack)?;
    std::fs::remove_file(root.join("merged.tar")).map_err(|_| Status::Unpack)?;
    Ok(output.bytes)
}

#[cfg(test)]
mod tests {
    use super::*;
    fn layer(root: &Path, index: usize, entries: &[(&str, &str)]) -> LayerBlob {
        let path = root.join(format!("layer-{index}"));
        let mut tar = tar::Builder::new(File::create(&path).unwrap());
        for (name, bytes) in entries {
            let mut h = tar::Header::new_ustar();
            h.set_mode(0o6755);
            h.set_size(bytes.len() as u64);
            h.set_cksum();
            tar.append_data(&mut h, name, bytes.as_bytes()).unwrap();
        }
        tar.finish().unwrap();
        LayerBlob {
            path,
            media_type: "application/vnd.oci.image.layer.v1.tar".into(),
            index,
        }
    }
    #[test]
    fn library_merge_and_unpack_strip_special_permissions() {
        use std::os::unix::fs::PermissionsExt;
        let root = tempfile::tempdir().unwrap();
        let a = layer(root.path(), 0, &[("kept", "old"), ("gone", "old")]);
        let b = layer(root.path(), 1, &[("kept", "new"), (".wh.gone", "")]);
        assert!(unpack(vec![a, b], root.path(), 65536).unwrap() > 0);
        assert_eq!(
            std::fs::read_to_string(root.path().join("rootfs/kept")).unwrap(),
            "new"
        );
        assert!(!root.path().join("rootfs/gone").exists());
        assert_eq!(
            std::fs::metadata(root.path().join("rootfs/kept"))
                .unwrap()
                .permissions()
                .mode()
                & 0o7000,
            0
        );
        assert!(!root.path().join("merged.tar").exists());
    }
    #[test]
    fn merge_output_budget_fails_before_extracting() {
        let root = tempfile::tempdir().unwrap();
        let a = layer(root.path(), 0, &[("large", &"a".repeat(4096))]);
        assert_eq!(unpack(vec![a], root.path(), 1024), Err(Status::Quota));
        assert!(!root.path().join("rootfs").exists());
    }
    #[test]
    fn malicious_link_and_parent_entries_do_not_escape_staging() {
        let fixture = tempfile::tempdir().unwrap();
        let outside = fixture.path().join("outside");
        std::fs::create_dir(&outside).unwrap();
        std::fs::write(outside.join("sentinel"), b"preserve").unwrap();
        for (index, kind) in [
            tar::EntryType::Symlink,
            tar::EntryType::Link,
            tar::EntryType::Regular,
        ]
        .into_iter()
        .enumerate()
        {
            let staging = fixture.path().join(format!("staging-{index}"));
            std::fs::create_dir(&staging).unwrap();
            let path = staging.join("bad.tar");
            let mut tar = tar::Builder::new(File::create(&path).unwrap());
            if !kind.is_file() {
                let mut h = tar::Header::new_ustar();
                h.set_entry_type(kind);
                h.set_mode(0o777);
                h.set_size(0);
                h.set_link_name(if kind.is_symlink() {
                    outside.clone()
                } else {
                    outside.join("sentinel")
                })
                .unwrap();
                h.set_cksum();
                tar.append_data(&mut h, "link", &b""[..]).unwrap();
            }
            if kind.is_symlink() {
                let mut h = tar::Header::new_ustar();
                h.set_mode(0o600);
                h.set_size(7);
                h.set_cksum();
                tar.append_data(&mut h, "link/sentinel", &b"changed"[..])
                    .unwrap();
            }
            if kind.is_file() {
                let mut h = tar::Header::new_ustar();
                h.as_mut_bytes()[..19].copy_from_slice(b"../outside/sentinel");
                h.set_mode(0o600);
                h.set_size(7);
                h.set_cksum();
                tar.append(&h, &b"changed"[..]).unwrap();
            }
            tar.finish().unwrap();
            let _ = unpack(
                vec![LayerBlob {
                    path,
                    media_type: "application/vnd.oci.image.layer.v1.tar".into(),
                    index: 0,
                }],
                &staging,
                65536,
            );
            assert_eq!(
                std::fs::read(outside.join("sentinel")).unwrap(),
                b"preserve"
            );
            if kind.is_hard_link() {
                assert!(!staging.join("rootfs/link").exists());
            }
        }
    }
}
