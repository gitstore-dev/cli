//! HTTP policy only: OCI and authentication protocol remain owned by oci-client.
//! This probe permits one numeric loopback origin; production consent must bind
//! registry, token-service and blob origins separately and require HTTPS.
use oci_client::transport::{self, Body, BoxError, EffectiveUrl, Request, Transport};
use std::{net::SocketAddr, sync::Arc, time::Duration};

#[derive(Clone)]
pub struct Policy {
    origin: String,
}
impl Policy {
    pub fn fixture(address: SocketAddr) -> Self {
        assert!(address.ip().is_loopback() && address.port() != 0);
        Self {
            origin: format!("http://{address}"),
        }
    }
    fn permits(&self, url: &reqwest::Url) -> bool {
        url.origin().ascii_serialization() == self.origin
            && url.username().is_empty()
            && url.password().is_none()
    }
}
pub fn transport(policy: Policy) -> Result<Transport, BoxError> {
    let policy = Arc::new(policy);
    let redirect_policy = policy.clone();
    let builder = reqwest::Client::builder()
        .no_proxy()
        .connect_timeout(Duration::from_secs(2))
        .timeout(Duration::from_secs(5))
        .redirect(reqwest::redirect::Policy::custom(move |attempt| {
            if attempt.previous().len() >= 5 || !redirect_policy.permits(attempt.url()) {
                attempt.error("unapproved fixture redirect")
            } else {
                attempt.follow()
            }
        }));
    // Select explicitly: feature unification must not silently select TLS.
    #[cfg(feature = "native")]
    let builder = builder.tls_backend_native();
    #[cfg(all(not(feature = "native"), feature = "rustls"))]
    let builder = builder.tls_backend_rustls();
    let client = builder.build()?;
    Ok(tower::util::BoxCloneSyncService::new(tower::service_fn(
        move |request: Request| {
            let policy = policy.clone();
            let client = client.clone();
            async move {
                let request = reqwest::Request::try_from(request)?;
                if !policy.permits(request.url()) {
                    return Err::<transport::Response, BoxError>(
                        std::io::Error::other("unapproved fixture origin").into(),
                    );
                }
                let response = client.execute(request).await?;
                let uri = response.url().as_str().parse::<http::Uri>()?;
                let mut response = http::Response::from(response).map(Body::wrap);
                response.extensions_mut().insert(EffectiveUrl(uri));
                Ok::<_, BoxError>(response)
            }
        },
    )))
}
