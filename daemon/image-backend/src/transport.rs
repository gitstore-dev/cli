use crate::{Result, Status, plan::Plan};
use oci_client::transport::{Body, BoxError, EffectiveUrl, Request, Transport};
use reqwest::Url;
use std::sync::{
    Arc,
    atomic::{AtomicU64, Ordering},
};
use std::time::Duration;

#[derive(Clone)]
pub(crate) struct Budget(Arc<AtomicU64>);
impl Budget {
    pub fn metadata() -> Self {
        Self(Arc::new(AtomicU64::new(1024 * 1024)))
    }
    pub fn set(&self, bytes: u64) {
        self.0.store(bytes, Ordering::Relaxed);
    }
}
pub(crate) fn build(plan: &Plan, budget: Budget) -> Result<Transport> {
    build_using(plan, budget, |builder| builder)
}
fn build_using(
    plan: &Plan,
    budget: Budget,
    configure: impl FnOnce(reqwest::ClientBuilder) -> reqwest::ClientBuilder,
) -> Result<Transport> {
    let reference = plan.validate()?;
    let registry = format!("https://{}", reference.resolve_registry());
    let mut credential_origins = plan.token_origins.clone();
    credential_origins.push(registry);
    let mut destinations = credential_origins.clone();
    destinations.extend(plan.blob_origins.clone());
    let destinations = Arc::new(destinations);
    let allowed_redirects = destinations.clone();
    let builder = reqwest::Client::builder()
        .tls_backend_native()
        .no_proxy()
        .connect_timeout(Duration::from_secs(10))
        .timeout(Duration::from_millis(plan.timeout_ms))
        .redirect(reqwest::redirect::Policy::custom(move |attempt| {
            if attempt.previous().len() >= 5 || !permits(&allowed_redirects, attempt.url()) {
                attempt.error("Unapproved image origin")
            } else {
                attempt.follow()
            }
        }));
    let client = configure(builder)
        .build()
        .map_err(|_| Status::Unavailable)?;
    Ok(tower::util::BoxCloneSyncService::new(tower::service_fn(
        move |request: Request| {
            let client = client.clone();
            let destinations = destinations.clone();
            let credential_origins = credential_origins.clone();
            let budget = budget.clone();
            async move {
                // Check the original URI before any HTTP client conversion can
                // normalize userinfo or translate it to a credential header.
                let target = reqwest::Url::parse(&request.uri().to_string())?;
                if !permits(&destinations, &target)
                    || (request.headers().contains_key(http::header::AUTHORIZATION)
                        && !permits(&credential_origins, &target))
                {
                    return Err::<http::Response<Body>, BoxError>(
                        std::io::Error::other("Unapproved image origin").into(),
                    );
                }
                let request = reqwest::Request::try_from(request)?;
                if !permits(&destinations, request.url())
                    || (request.headers().contains_key(http::header::AUTHORIZATION)
                        && !permits(&credential_origins, request.url()))
                {
                    return Err::<http::Response<Body>, BoxError>(
                        std::io::Error::other("Unapproved image origin").into(),
                    );
                }
                let response = client.execute(request).await?;
                let uri = response.url().as_str().parse::<http::Uri>()?;
                let limit = usize::try_from(budget.0.load(Ordering::Relaxed))?;
                let mut response = http::Response::from(response)
                    .map(|body| Body::wrap(http_body_util::Limited::new(body, limit)));
                response.extensions_mut().insert(EffectiveUrl(uri));
                Ok::<_, BoxError>(response)
            }
        },
    )))
}
fn permits(origins: &[String], url: &Url) -> bool {
    url.scheme() == "https"
        && url.username().is_empty()
        && url.password().is_none()
        && origins.contains(&url.origin().ascii_serialization())
}

#[cfg(test)]
mod tests {
    use super::*;
    use tower::ServiceExt;
    #[test]
    #[ignore = "explicit owned synthetic TLS redirect fixture only"]
    fn fixture_redirects_strip_credentials_and_reject_unapproved_origins() {
        #[derive(serde::Deserialize)]
        #[serde(deny_unknown_fields)]
        struct Fixture {
            source: String,
            destination: String,
            certificates: Vec<String>,
        }
        let fixture: Fixture =
            serde_json::from_str(&std::env::var("GITSTORE_TEST_IMAGE_REDIRECT_FIXTURE").unwrap())
                .unwrap();
        for origin in [&fixture.source, &fixture.destination] {
            let parsed = Url::parse(origin).unwrap();
            assert_eq!(parsed.scheme(), "https");
            assert_eq!(parsed.host_str(), Some("127.0.0.1"));
            assert!(parsed.port().is_some());
        }
        let plan = Plan {
            schema_version: 1,
            reference: format!(
                "{}/owned/image@sha256:{}",
                fixture.source.trim_start_matches("https://"),
                "a".repeat(64)
            ),
            config_digest: format!("sha256:{}", "b".repeat(64)),
            os: "linux".into(),
            architecture: "arm64".into(),
            token_origins: vec![],
            blob_origins: vec![fixture.destination],
            maximum_download_bytes: 1024,
            maximum_merged_bytes: 4096,
            timeout_ms: 3000,
        };
        let runtime = tokio::runtime::Builder::new_current_thread()
            .enable_all()
            .build()
            .unwrap();
        runtime.block_on(async {
            let transport = build_using(&plan, Budget::metadata(), |mut builder| {
                for certificate in &fixture.certificates {
                    builder = builder.add_root_certificate(
                        reqwest::Certificate::from_pem(certificate.as_bytes()).unwrap(),
                    );
                }
                builder
            })
            .unwrap();
            for path in ["/redirect", "/downgrade", "/foreign", "/loop"] {
                let request = http::Request::builder()
                    .uri(format!("{}{path}", fixture.source))
                    .header(http::header::AUTHORIZATION, "Bearer synthetic-only")
                    .body(Body::empty())
                    .unwrap();
                let result = transport.clone().oneshot(request).await;
                if path == "/redirect" {
                    assert_eq!(result.unwrap().status(), http::StatusCode::OK);
                } else {
                    assert!(result.is_err());
                }
            }
        });
    }
    #[test]
    fn approved_blob_destination_cannot_receive_credentials() {
        let plan = Plan {
            schema_version: 1,
            reference: format!("registry.example/owned/image@sha256:{}", "a".repeat(64)),
            config_digest: format!("sha256:{}", "b".repeat(64)),
            os: "linux".into(),
            architecture: "arm64".into(),
            token_origins: vec!["https://tokens.example".into()],
            blob_origins: vec!["https://blobs.example".into()],
            maximum_download_bytes: 1024,
            maximum_merged_bytes: 4096,
            timeout_ms: 1000,
        };
        let budget = Budget::metadata();
        budget.set(1024);
        let runtime = tokio::runtime::Builder::new_current_thread()
            .enable_all()
            .build()
            .unwrap();
        runtime.block_on(async {
            let request = http::Request::builder()
                .uri("https://blobs.example/owned")
                .header(http::header::AUTHORIZATION, "Bearer synthetic-only")
                .body(Body::empty())
                .unwrap();
            let error = build(&plan, budget)
                .unwrap()
                .oneshot(request)
                .await
                .unwrap_err();
            assert_eq!(error.to_string(), "Unapproved image origin");
        });
        assert!(!permits(
            &plan.blob_origins,
            &Url::parse("http://blobs.example/owned").unwrap()
        ));
        assert!(!permits(
            &plan.blob_origins,
            &Url::parse("https://user:secret@blobs.example/owned").unwrap()
        ));
    }
}
