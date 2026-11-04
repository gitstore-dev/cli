//! Isolated library API/size probe, never a production capability registration.
use oci_client::{
    Client, Reference,
    client::{ClientConfig, ClientProtocol},
    secrets::RegistryAuth,
};
use ocirender::image::LayerBlob;
use std::{
    path::{Path, PathBuf},
    sync::mpsc,
    time::Duration,
};
pub mod policy;

pub fn unpack_layers(layers: Vec<LayerBlob>, output: &Path) -> Result<(), String> {
    if output.exists() {
        return Err("Select a new owned fixture output".into());
    }
    let count = layers.len();
    let (sender, receiver) = mpsc::channel();
    for item in layers {
        sender
            .send(Ok(item))
            .map_err(|_| "fixture channel failed")?;
    }
    drop(sender);
    ocirender::dir::write_dir(receiver, count, output).map_err(|_| "Candidate unpack failed".into())
}
pub fn fetch_fixture(reference: &str, output: &Path) -> Result<(), String> {
    let reference: Reference = reference.parse().map_err(|_| "Invalid fixture reference")?;
    let address: std::net::SocketAddr = reference
        .registry()
        .parse()
        .map_err(|_| "Use a numeric loopback fixture registry")?;
    if !address.ip().is_loopback() || address.port() == 0 || reference.digest().is_none() {
        return Err("Use a digest-pinned loopback fixture only".into());
    }
    let runtime = tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
        .map_err(|_| "Cannot create fixture runtime")?;
    let image = runtime.block_on(async {
        let client = Client::new_with_transport(
            ClientConfig {
                protocol: ClientProtocol::HttpsExcept(vec![reference.registry().into()]),
                read_timeout: Some(Duration::from_secs(2)),
                connect_timeout: Some(Duration::from_secs(2)),
                max_concurrent_download: 2,
                ..Default::default()
            },
            policy::transport(policy::Policy::fixture(address))
                .map_err(|_| "Cannot configure fixture transport")?,
        );
        tokio::time::timeout(
            Duration::from_secs(5),
            client.pull(
                &reference,
                &RegistryAuth::Basic("synthetic".into(), "fixture-only".into()),
                vec![
                    oci_client::manifest::IMAGE_LAYER_MEDIA_TYPE,
                    oci_client::manifest::IMAGE_LAYER_GZIP_MEDIA_TYPE,
                ],
            ),
        )
        .await
        .map_err(|_| "Fixture deadline exceeded")?
        .map_err(|_| "Fixture transfer failed")
    })?;
    if image.digest.as_deref() != reference.digest() {
        return Err("Fixture manifest digest mismatch".into());
    }
    // The full-buffer pull is only for tiny known synthetic fixtures. A product
    // adapter must use bounded streaming, disk/memory quotas and verified activation.
    if image.layers.len() > 8 || image.layers.iter().any(|l| l.data.len() > 1024 * 1024) {
        return Err("Fixture exceeds experiment bounds".into());
    }
    let root = tempfile::Builder::new()
        .prefix("gitstore-rust-image-")
        .tempdir()
        .map_err(|_| "Cannot create private fixture staging")?;
    let mut layers = Vec::new();
    for (index, layer) in image.layers.into_iter().enumerate() {
        let path = root.path().join(format!("layer-{index}"));
        std::fs::write(&path, layer.data).map_err(|_| "Cannot write owned fixture blob")?;
        layers.push(LayerBlob {
            path,
            media_type: layer.media_type,
            index,
        });
    }
    unpack_layers(layers, output)
}
unsafe fn text<'a>(pointer: *const u8, length: usize) -> Result<&'a str, ()> {
    if pointer.is_null() || length == 0 || length > 4096 {
        return Err(());
    }
    let bytes = unsafe { std::slice::from_raw_parts(pointer, length) };
    std::str::from_utf8(bytes).map_err(|_| ())
}
/// Experiment-only C bridge. All buffers must remain valid for their given lengths.
/// # Safety
/// Caller supplies valid buffers, a tiny owned loopback registry and new private output.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn gitstore_rust_image_probe(
    reference: *const u8,
    reference_len: usize,
    output: *const u8,
    output_len: usize,
) -> i32 {
    std::panic::catch_unwind(|| {
        let reference = unsafe { text(reference, reference_len) }?;
        let output = PathBuf::from(unsafe { text(output, output_len) }?);
        fetch_fixture(reference, &output).map_err(|_| ())
    })
    .map_or(1, |result| if result.is_ok() { 0 } else { 1 })
}
