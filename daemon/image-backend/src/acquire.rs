use crate::{
    Outcome, Result, Status,
    plan::{self, Plan},
    resume::CachedBlobs,
    transport, unpack,
};
use futures_util::TryStreamExt;
use oci_client::{
    Client, Reference,
    client::ClientConfig,
    manifest::{self, OciDescriptor, OciImageManifest},
    secrets::RegistryAuth,
};
use ocirender::image::LayerBlob;
use sha2::{Digest, Sha256};
use std::{
    fs::{File, OpenOptions},
    io::{Read, Write},
    os::unix::fs::OpenOptionsExt,
    path::Path,
    time::Duration,
};
pub(crate) fn sha256(bytes: &[u8]) -> String {
    format!("sha256:{}", hex::encode(Sha256::digest(bytes)))
}
pub(crate) fn descriptor(d: &OciDescriptor) -> Result<u64> {
    if !plan::digest(&d.digest) || d.urls.as_ref().is_some_and(|u| !u.is_empty()) || d.size <= 0 {
        return Err(Status::Integrity);
    }
    u64::try_from(d.size).map_err(|_| Status::Integrity)
}
pub(crate) fn write_new(path: &Path, bytes: &[u8]) -> Result<()> {
    let mut file = OpenOptions::new()
        .write(true)
        .create_new(true)
        .mode(0o600)
        .open(path)
        .map_err(|_| Status::Transfer)?;
    file.write_all(bytes)
        .and_then(|()| file.sync_all())
        .map_err(|_| Status::Transfer)
}
async fn blob(
    client: &Client,
    reference: &Reference,
    d: &OciDescriptor,
    path: &Path,
    remaining: &mut u64,
    budget: &transport::Budget,
    cached: Option<(&CachedBlobs, &str)>,
) -> Result<()> {
    let expected = descriptor(d)?;
    if expected > *remaining {
        return Err(Status::Quota);
    }
    if let Some((source, name)) = cached
        && source.copy(d, name, path, remaining)?
    {
        return Ok(());
    }
    budget.set(expected);
    let mut stream = client
        .pull_blob_stream(reference, d)
        .await
        .map_err(|_| Status::Transfer)?;
    if stream.content_length.is_some_and(|n| n != expected) {
        return Err(Status::Integrity);
    }
    let mut file = OpenOptions::new()
        .write(true)
        .create_new(true)
        .mode(0o600)
        .open(path)
        .map_err(|_| Status::Transfer)?;
    let mut count = 0u64;
    while let Some(bytes) = stream.try_next().await.map_err(|_| Status::Integrity)? {
        count = count.checked_add(bytes.len() as u64).ok_or(Status::Quota)?;
        if count > expected {
            return Err(Status::Integrity);
        }
        file.write_all(&bytes).map_err(|_| Status::Transfer)?;
    }
    // Consuming the library stream through EOF verifies the content digest.
    if count != expected {
        return Err(Status::Integrity);
    }
    file.sync_all().map_err(|_| Status::Transfer)?;
    *remaining -= count;
    Ok(())
}
pub(crate) fn verified_manifest(plan: &Plan, raw: &[u8]) -> Result<OciImageManifest> {
    let reference = plan.validate()?;
    if raw.len() > 1024 * 1024 || Some(sha256(raw).as_str()) != reference.digest() {
        return Err(Status::Integrity);
    }
    let manifest: OciImageManifest = serde_json::from_slice(raw).map_err(|_| Status::Integrity)?;
    if manifest.schema_version != 2
        || manifest.layers.len() > 128
        || manifest.config.digest != plan.config_digest
        || descriptor(&manifest.config)? > 1024 * 1024
        || !matches!(
            manifest.config.media_type.as_str(),
            manifest::IMAGE_CONFIG_MEDIA_TYPE | manifest::IMAGE_DOCKER_CONFIG_MEDIA_TYPE
        )
        || manifest.media_type.as_deref().is_some_and(|t| {
            !matches!(
                t,
                manifest::OCI_IMAGE_MEDIA_TYPE | manifest::IMAGE_MANIFEST_MEDIA_TYPE
            )
        })
    {
        return Err(Status::Integrity);
    }
    let mut required = descriptor(&manifest.config)?;
    for d in &manifest.layers {
        if !matches!(
            d.media_type.as_str(),
            manifest::IMAGE_LAYER_MEDIA_TYPE
                | manifest::IMAGE_LAYER_GZIP_MEDIA_TYPE
                | manifest::IMAGE_DOCKER_LAYER_TAR_MEDIA_TYPE
                | manifest::IMAGE_DOCKER_LAYER_GZIP_MEDIA_TYPE
                | "application/vnd.oci.image.layer.v1.tar+zstd"
        ) {
            return Err(Status::Integrity);
        }
        required = required.checked_add(descriptor(d)?).ok_or(Status::Quota)?;
    }
    if required > plan.maximum_download_bytes {
        return Err(Status::Quota);
    }
    Ok(manifest)
}
pub(crate) fn verified_config(plan: &Plan, config: &[u8]) -> Result<()> {
    if config.len() > 1024 * 1024 {
        return Err(Status::Integrity);
    }
    if sha256(config) != plan.config_digest {
        return Err(Status::Integrity);
    }
    let config: serde_json::Value =
        serde_json::from_slice(config).map_err(|_| Status::Integrity)?;
    if config.get("os").and_then(|v| v.as_str()) != Some(plan.os.as_str())
        || config.get("architecture").and_then(|v| v.as_str()) != Some(plan.architecture.as_str())
    {
        return Err(Status::Integrity);
    }
    Ok(())
}
async fn download(
    plan: &Plan,
    root: &Path,
    auth: &RegistryAuth,
    cached: Option<&CachedBlobs>,
) -> Result<(Vec<LayerBlob>, u64)> {
    let budget = transport::Budget::metadata();
    let client = Client::new_with_transport(
        ClientConfig::default(),
        transport::build(plan, budget.clone())?,
    );
    download_with_cache(plan, root, auth, &client, &budget, cached).await
}
#[cfg(test)]
async fn download_using(
    plan: &Plan,
    root: &Path,
    auth: &RegistryAuth,
    client: &Client,
    budget: &transport::Budget,
) -> Result<(Vec<LayerBlob>, u64)> {
    download_with_cache(plan, root, auth, client, budget, None).await
}
async fn download_with_cache(
    plan: &Plan,
    root: &Path,
    auth: &RegistryAuth,
    client: &Client,
    budget: &transport::Budget,
    cached: Option<&CachedBlobs>,
) -> Result<(Vec<LayerBlob>, u64)> {
    let reference = plan.validate()?;
    budget.set(1024 * 1024);
    let (raw, returned_digest) = if let Some(source) = cached {
        (
            source.manifest().to_vec(),
            reference.digest().ok_or(Status::Invalid)?.to_owned(),
        )
    } else {
        let (raw, digest) = client
            .pull_manifest_raw(
                &reference,
                auth,
                &[
                    manifest::OCI_IMAGE_MEDIA_TYPE,
                    manifest::IMAGE_MANIFEST_MEDIA_TYPE,
                ],
            )
            .await
            .map_err(|_| Status::Transfer)?;
        (raw.to_vec(), digest)
    };
    if raw.len() > 1024 * 1024
        || Some(returned_digest.as_str()) != reference.digest()
        || Some(sha256(&raw).as_str()) != reference.digest()
    {
        return Err(Status::Integrity);
    }
    let manifest = verified_manifest(plan, &raw)?;
    std::fs::create_dir(root.join("blobs")).map_err(|_| Status::Transfer)?;
    write_new(&root.join("manifest.json"), &raw)?;
    let mut remaining = plan.maximum_download_bytes;
    blob(
        client,
        &reference,
        &manifest.config,
        &root.join("config.json"),
        &mut remaining,
        budget,
        cached.map(|source| (source, "config.json")),
    )
    .await?;
    let mut config = Vec::new();
    File::open(root.join("config.json"))
        .map_err(|_| Status::Integrity)?
        .take(1024 * 1024 + 1)
        .read_to_end(&mut config)
        .map_err(|_| Status::Integrity)?;
    verified_config(plan, &config)?;
    let mut layers = Vec::new();
    for (index, d) in manifest.layers.iter().enumerate() {
        // Layer indices, never caller paths or untrusted archive layout filenames.
        let path = root.join("blobs").join(format!("layer-{index}"));
        let name = format!("blobs/layer-{index}");
        blob(
            client,
            &reference,
            d,
            &path,
            &mut remaining,
            budget,
            cached.map(|source| (source, name.as_str())),
        )
        .await?;
        layers.push(LayerBlob {
            path,
            media_type: d.media_type.clone(),
            index,
        });
    }
    Ok((layers, plan.maximum_download_bytes - remaining))
}
pub(crate) fn stage(plan: &Plan, root: &Path, auth: &RegistryAuth) -> Result<Outcome> {
    stage_cached(plan, root, auth, None)
}
pub(crate) fn stage_cached(
    plan: &Plan,
    root: &Path,
    auth: &RegistryAuth,
    cached: Option<&CachedBlobs>,
) -> Result<Outcome> {
    let runtime = tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
        .map_err(|_| Status::Unavailable)?;
    let (layers, downloaded_bytes) = runtime.block_on(async {
        tokio::time::timeout(
            Duration::from_millis(plan.timeout_ms),
            download(plan, root, auth, cached),
        )
        .await
        .map_err(|_| Status::Transfer)?
    })?;
    drop(runtime);
    let merged_bytes = unpack::unpack(layers, root, plan.maximum_merged_bytes)?;
    Ok(Outcome {
        downloaded_bytes,
        merged_bytes,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn downloaded_files_stay_private_with_permissive_umask() {
        // umask is process-global: isolate this case from parallel SDK tests.
        if std::env::var("GSTD_IMAGE_MODE_CHILD").as_deref() != Ok("1") {
            assert!(
                std::process::Command::new(std::env::current_exe().unwrap())
                    .args([
                        "--exact",
                        "acquire::tests::downloaded_files_stay_private_with_permissive_umask",
                        "--test-threads=1"
                    ])
                    .env("GSTD_IMAGE_MODE_CHILD", "1")
                    .status()
                    .unwrap()
                    .success()
            );
            return;
        }
        unsafe {
            libc::umask(0o002);
        }
        let root = tempfile::tempdir().unwrap();
        write_new(&root.path().join("manifest.json"), b"synthetic manifest").unwrap();
        use std::os::unix::fs::PermissionsExt;
        assert_eq!(
            std::fs::metadata(root.path().join("manifest.json"))
                .unwrap()
                .permissions()
                .mode()
                & 0o777,
            0o600
        );
        let data = b"synthetic registry layer";
        let descriptor = OciDescriptor {
            digest: sha256(data),
            size: data.len() as i64,
            media_type: manifest::IMAGE_LAYER_MEDIA_TYPE.into(),
            ..Default::default()
        };
        let service =
            tower::service_fn(move |request: oci_client::transport::Request| async move {
                assert_eq!(request.method(), http::Method::GET);
                Ok::<_, oci_client::transport::BoxError>(http::Response::new(
                    oci_client::transport::Body::from_bytes(data.as_slice()),
                ))
            });
        let client = Client::new_with_transport(ClientConfig::default(), service);
        let reference: Reference = format!("example.invalid/synthetic/image@{}", descriptor.digest)
            .parse()
            .unwrap();
        let runtime = tokio::runtime::Builder::new_current_thread()
            .enable_all()
            .build()
            .unwrap();
        let path = root.path().join("layer");
        let mut remaining = 65536;
        runtime
            .block_on(blob(
                &client,
                &reference,
                &descriptor,
                &path,
                &mut remaining,
                &transport::Budget::metadata(),
                None,
            ))
            .unwrap();
        assert_eq!(
            std::fs::metadata(&path).unwrap().permissions().mode() & 0o777,
            0o600
        );
        assert_eq!(std::fs::read(path).unwrap(), data);
    }
    #[test]
    #[ignore = "explicit owned synthetic registry fixture only"]
    fn fixture_streams_verify_platform_quota_and_unpack() {
        let reference =
            std::env::var("GITSTORE_TEST_IMAGE_REFERENCE").expect("explicit fixture reference");
        let config_digest = std::env::var("GITSTORE_TEST_IMAGE_CONFIG_DIGEST")
            .expect("explicit fixture config digest");
        let parsed: Reference = reference.parse().unwrap();
        let address: std::net::SocketAddr =
            parsed.registry().parse().expect("numeric loopback fixture");
        assert!(address.ip().is_loopback() && address.port() != 0);
        let mut plan = Plan {
            schema_version: 1,
            reference,
            config_digest,
            os: "linux".into(),
            architecture: "arm64".into(),
            token_origins: vec![],
            blob_origins: vec![],
            maximum_download_bytes: 65536,
            maximum_merged_bytes: 65536,
            timeout_ms: 5000,
        };
        plan.validate().unwrap();
        let runtime = tokio::runtime::Builder::new_current_thread()
            .enable_all()
            .build()
            .unwrap();
        runtime.block_on(async {
            // Test-only injection. No C ABI/production option can select HTTP.
            let http = reqwest::Client::builder()
                .no_proxy()
                .redirect(reqwest::redirect::Policy::none())
                .timeout(Duration::from_secs(3))
                .build()
                .unwrap();
            let policy_origin = format!("http://{address}");
            let requests = std::sync::Arc::new(std::sync::Mutex::new(Vec::new()));
            let observed = requests.clone();
            let service = tower::service_fn(move |request: oci_client::transport::Request| {
                observed.lock().unwrap().push((
                    request.uri().path().to_owned(),
                    request.headers().contains_key(http::header::RANGE),
                ));
                let http = http.clone();
                let policy_origin = policy_origin.clone();
                async move {
                    let request = reqwest::Request::try_from(request)?;
                    if request.url().origin().ascii_serialization() != policy_origin {
                        return Err::<
                            http::Response<oci_client::transport::Body>,
                            oci_client::transport::BoxError,
                        >(
                            std::io::Error::other("fixture origin").into()
                        );
                    }
                    let response = http.execute(request).await?;
                    Ok::<_, oci_client::transport::BoxError>(http::Response::from(response).map(
                        |body| {
                            oci_client::transport::Body::wrap(http_body_util::Limited::new(
                                body, 65536,
                            ))
                        },
                    ))
                }
            });
            let client = Client::new_with_transport(
                ClientConfig {
                    protocol: oci_client::client::ClientProtocol::Http,
                    ..Default::default()
                },
                service.clone(),
            );
            // Fresh maintained clients prevent an earlier successful registry
            // session from masking anonymous or incorrect credential rejection.
            for denied in [
                RegistryAuth::Anonymous,
                RegistryAuth::Basic("synthetic".into(), "wrong-fixture-only".into()),
                RegistryAuth::Basic("other-fixture-user".into(), "fixture-only".into()),
            ] {
                let rejected = tempfile::tempdir().unwrap();
                let unauthenticated = Client::new_with_transport(
                    ClientConfig {
                        protocol: oci_client::client::ClientProtocol::Http,
                        ..Default::default()
                    },
                    service.clone(),
                );
                assert!(
                    download_using(
                        &plan,
                        rejected.path(),
                        &denied,
                        &unauthenticated,
                        &transport::Budget::metadata(),
                    )
                    .await
                    .is_err()
                );
                assert!(std::fs::read_dir(rejected.path()).unwrap().next().is_none());
            }
            let credentials = RegistryAuth::Basic("synthetic".into(), "fixture-only".into());
            if std::env::var("GITSTORE_TEST_IMAGE_AUTH_FAILURE").as_deref() == Ok("1") {
                let rejected = tempfile::tempdir().unwrap();
                assert!(
                    download_using(
                        &plan,
                        rejected.path(),
                        &credentials,
                        &client,
                        &transport::Budget::metadata(),
                    )
                    .await
                    .is_err()
                );
                assert!(std::fs::read_dir(rejected.path()).unwrap().next().is_none());
                return;
            }
            let root = tempfile::tempdir().unwrap();
            use std::os::unix::fs::PermissionsExt;
            std::fs::set_permissions(root.path(), std::fs::Permissions::from_mode(0o700)).unwrap();
            let (layers, bytes) = download_using(
                &plan,
                root.path(),
                &credentials,
                &client,
                &transport::Budget::metadata(),
            )
            .await
            .unwrap();
            assert!(bytes > 0 && bytes <= plan.maximum_download_bytes);
            unpack::unpack(layers, root.path(), plan.maximum_merged_bytes).unwrap();
            assert_eq!(
                std::fs::read_to_string(root.path().join("rootfs/retained")).unwrap(),
                "two"
            );
            assert!(!root.path().join("rootfs/gone").exists());
            // Reuse intact blobs; without a strong HTTP range validator the
            // truncated last layer must be fetched in full, never appended.
            let manifest: OciImageManifest =
                serde_json::from_slice(&std::fs::read(root.path().join("manifest.json")).unwrap())
                    .unwrap();
            let last = manifest.layers.len() - 1;
            let partial = root.path().join("blobs").join(format!("layer-{last}"));
            OpenOptions::new()
                .write(true)
                .open(&partial)
                .unwrap()
                .set_len(1)
                .unwrap();
            let resumed = tempfile::tempdir().unwrap();
            let source = File::open(root.path()).unwrap();
            use std::os::fd::AsRawFd;
            let cached = CachedBlobs::new(&plan, source.as_raw_fd(), resumed.path()).unwrap();
            requests.lock().unwrap().clear();
            let (layers, _) = download_with_cache(
                &plan,
                resumed.path(),
                &credentials,
                &client,
                &transport::Budget::metadata(),
                Some(&cached),
            )
            .await
            .unwrap();
            let blobs: Vec<_> = requests
                .lock()
                .unwrap()
                .iter()
                .filter(|(path, _)| path.contains("/blobs/"))
                .cloned()
                .collect();
            assert_eq!(blobs.len(), 1);
            assert!(blobs[0].0.ends_with(&manifest.layers[last].digest));
            assert!(
                !blobs[0].1,
                "partial bytes require a full restart without a range validator"
            );
            unpack::unpack(layers, resumed.path(), plan.maximum_merged_bytes).unwrap();
            assert_eq!(
                std::fs::read_to_string(resumed.path().join("rootfs/retained")).unwrap(),
                "two"
            );
            assert_eq!(std::fs::metadata(partial).unwrap().len(), 1);
            let wrong = tempfile::tempdir().unwrap();
            plan.architecture = "amd64".into();
            assert!(matches!(
                download_using(
                    &plan,
                    wrong.path(),
                    &credentials,
                    &client,
                    &transport::Budget::metadata()
                )
                .await,
                Err(Status::Integrity)
            ));
            assert!(!wrong.path().join("rootfs").exists());
            let over = tempfile::tempdir().unwrap();
            plan.architecture = "arm64".into();
            plan.maximum_download_bytes = 1;
            assert!(matches!(
                download_using(
                    &plan,
                    over.path(),
                    &credentials,
                    &client,
                    &transport::Budget::metadata()
                )
                .await,
                Err(Status::Quota)
            ));
            assert!(std::fs::read_dir(over.path()).unwrap().next().is_none());
        });
    }
}
