use crate::{Result, Status};
use oci_client::Reference;
use reqwest::Url;
use serde::Deserialize;

#[derive(Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub(crate) struct Plan {
    pub schema_version: u32,
    pub reference: String,
    pub config_digest: String,
    pub os: String,
    pub architecture: String,
    pub token_origins: Vec<String>,
    pub blob_origins: Vec<String>,
    pub maximum_download_bytes: u64,
    pub maximum_merged_bytes: u64,
    pub timeout_ms: u64,
}
pub(crate) fn digest(s: &str) -> bool {
    s.len() == 71
        && s.starts_with("sha256:")
        && s[7..]
            .bytes()
            .all(|c| c.is_ascii_digit() || (b'a'..=b'f').contains(&c))
}
pub(crate) fn origin(s: &str) -> Result<String> {
    if s.len() > 1024 {
        return Err(Status::Invalid);
    }
    let url = Url::parse(s).map_err(|_| Status::Invalid)?;
    if url.scheme() != "https"
        || url.host_str().is_none()
        || !url.username().is_empty()
        || url.password().is_some()
        || url.path() != "/"
        || url.query().is_some()
        || url.fragment().is_some()
        || s != url.origin().ascii_serialization()
    {
        return Err(Status::Invalid);
    }
    Ok(s.to_owned())
}
impl Plan {
    pub fn parse(bytes: &[u8]) -> Result<Self> {
        if bytes.is_empty() || bytes.len() > 65536 {
            return Err(Status::Invalid);
        }
        let p: Self = serde_json::from_slice(bytes).map_err(|_| Status::Invalid)?;
        p.validate()?;
        Ok(p)
    }
    pub fn validate(&self) -> Result<Reference> {
        if self.schema_version != 1
            || self.reference.len() > 4096
            || !digest(&self.config_digest)
            || self.os != "linux"
            || !matches!(self.architecture.as_str(), "amd64" | "arm64")
            || self.token_origins.len() > 8
            || self.blob_origins.len() > 8
            || !(1..=2 * 1024 * 1024 * 1024).contains(&self.maximum_download_bytes)
            || !(1024..=8 * 1024 * 1024 * 1024).contains(&self.maximum_merged_bytes)
            || !(1..=600_000).contains(&self.timeout_ms)
        {
            return Err(Status::Invalid);
        }
        let reference: Reference = self.reference.parse().map_err(|_| Status::Invalid)?;
        let hash = reference.digest().ok_or(Status::Invalid)?;
        if !digest(hash)
            || self.reference
                != format!("{}/{}@{hash}", reference.registry(), reference.repository())
        {
            return Err(Status::Invalid);
        }
        origin(&format!("https://{}", reference.resolve_registry()))?;
        for item in self.token_origins.iter().chain(self.blob_origins.iter()) {
            origin(item)?;
        }
        Ok(reference)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn value() -> serde_json::Value {
        serde_json::json!({"schemaVersion":1,"reference":format!("ghcr.io/owned/image@sha256:{}","a".repeat(64)),"configDigest":format!("sha256:{}","b".repeat(64)),"os":"linux","architecture":"arm64","tokenOrigins":[],"blobOrigins":[],"maximumDownloadBytes":1024,"maximumMergedBytes":4096,"timeoutMs":1000})
    }
    #[test]
    fn strict_immutable_plan() {
        let v = value();
        assert!(Plan::parse(&serde_json::to_vec(&v).unwrap()).is_ok());
        let mut mutable = v.clone();
        mutable["reference"] = "ghcr.io/owned/image:latest".into();
        assert!(Plan::parse(&serde_json::to_vec(&mutable).unwrap()).is_err());
        let mut unknown = v;
        unknown["command"] = "arbitrary".into();
        assert!(Plan::parse(&serde_json::to_vec(&unknown).unwrap()).is_err());
        assert!(Plan::parse(b"{\"schemaVersion\":1,\"schemaVersion\":1}").is_err());
    }
    #[test]
    fn origins_cannot_embed_credentials_or_paths() {
        assert!(origin("https://registry.example").is_ok());
        for bad in [
            "http://registry.example",
            "https://user:secret@registry.example",
            "https://registry.example/token",
            "https://registry.example?key=secret",
            "https://registry.example#fragment",
            "https://registry.example/",
        ] {
            assert!(origin(bad).is_err());
        }
    }
}
