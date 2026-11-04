use gitstore_image_library_probe::unpack_layers;
use ocirender::image::LayerBlob;
use std::{fs, path::Path};
fn layer(root: &Path, index: usize, files: &[(&str, &str)]) -> LayerBlob {
    let path = root.join(format!("layer-{index}.tar"));
    let mut builder = tar::Builder::new(fs::File::create(&path).unwrap());
    for (name, data) in files {
        let mut header = tar::Header::new_ustar();
        header.set_mode(0o600);
        header.set_size(data.len() as u64);
        header.set_cksum();
        builder
            .append_data(&mut header, name, data.as_bytes())
            .unwrap();
    }
    builder.finish().unwrap();
    LayerBlob {
        path,
        media_type: "application/vnd.oci.image.layer.v1.tar".into(),
        index,
    }
}
#[test]
fn replacement_and_whiteout_are_library_owned() {
    let root = tempfile::tempdir().unwrap();
    let output = root.path().join("out");
    let layers = vec![
        layer(root.path(), 0, &[("retained", "one"), ("gone", "old")]),
        layer(root.path(), 1, &[("retained", "two"), (".wh.gone", "")]),
    ];
    unpack_layers(layers, &output).unwrap();
    assert_eq!(fs::read_to_string(output.join("retained")).unwrap(), "two");
    assert!(!output.join("gone").exists());
}
#[test]
fn malformed_layer_does_not_report_success() {
    let root = tempfile::tempdir().unwrap();
    let path = root.path().join("bad");
    fs::write(&path, b"truncated tar").unwrap();
    assert!(
        unpack_layers(
            vec![LayerBlob {
                path,
                media_type: "application/vnd.oci.image.layer.v1.tar".into(),
                index: 0
            }],
            &root.path().join("out")
        )
        .is_err()
    );
}
#[test]
fn symlink_targets_cannot_modify_owned_sibling() {
    let root = tempfile::tempdir().unwrap();
    let outside = root.path().join("outside");
    fs::create_dir(&outside).unwrap();
    fs::write(outside.join("sentinel"), "preserve").unwrap();
    let archive = root.path().join("symlink.tar");
    let mut builder = tar::Builder::new(fs::File::create(&archive).unwrap());
    let mut header = tar::Header::new_ustar();
    header.set_entry_type(tar::EntryType::Symlink);
    header.set_size(0);
    header.set_mode(0o700);
    header.set_link_name(&outside).unwrap();
    header.set_cksum();
    builder.append_data(&mut header, "link", &b""[..]).unwrap();
    let mut header = tar::Header::new_ustar();
    header.set_size(7);
    header.set_mode(0o600);
    header.set_cksum();
    builder
        .append_data(&mut header, "link/sentinel", &b"changed"[..])
        .unwrap();
    builder.finish().unwrap();
    let _ = unpack_layers(
        vec![LayerBlob {
            path: archive,
            media_type: "application/vnd.oci.image.layer.v1.tar".into(),
            index: 0,
        }],
        &root.path().join("out"),
    );
    assert_eq!(
        fs::read_to_string(outside.join("sentinel")).unwrap(),
        "preserve"
    );
}

#[test]
fn hardlink_targets_cannot_modify_owned_sibling() {
    let root = tempfile::tempdir().unwrap();
    let outside = root.path().join("outside");
    fs::write(&outside, "preserve").unwrap();
    let archive = root.path().join("hardlink.tar");
    let mut builder = tar::Builder::new(fs::File::create(&archive).unwrap());
    let mut header = tar::Header::new_ustar();
    header.set_entry_type(tar::EntryType::Link);
    header.set_size(0);
    header.set_mode(0o600);
    header.set_link_name(&outside).unwrap();
    header.set_cksum();
    builder.append_data(&mut header, "link", &b""[..]).unwrap();
    builder.finish().unwrap();
    let output = root.path().join("out");
    let _ = unpack_layers(
        vec![LayerBlob {
            path: archive,
            media_type: "application/vnd.oci.image.layer.v1.tar".into(),
            index: 0,
        }],
        &output,
    );
    // Even if a library chooses to skip the invalid link, it must not publish
    // an alias to a file outside the owned output root.
    assert!(!output.join("link").exists());
    assert_eq!(fs::read_to_string(outside).unwrap(), "preserve");
}

#[test]
fn parent_paths_cannot_escape_owned_staging() {
    let root = tempfile::tempdir().unwrap();
    fs::write(root.path().join("sentinel"), "preserve").unwrap();
    let archive = root.path().join("traversal.tar");
    let mut builder = tar::Builder::new(fs::File::create(&archive).unwrap());
    let mut header = tar::Header::new_ustar();
    // Deliberately malformed input; tar's normal path setter rejects this.
    header.as_mut_bytes()[..11].copy_from_slice(b"../sentinel");
    header.set_size(7);
    header.set_mode(0o600);
    header.set_cksum();
    builder.append(&header, &b"changed"[..]).unwrap();
    builder.finish().unwrap();
    let _ = unpack_layers(
        vec![LayerBlob {
            path: archive,
            media_type: "application/vnd.oci.image.layer.v1.tar".into(),
            index: 0,
        }],
        &root.path().join("out"),
    );
    assert_eq!(
        fs::read_to_string(root.path().join("sentinel")).unwrap(),
        "preserve"
    );
}
