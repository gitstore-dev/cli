use crate::{
    cli::Cli,
    error::{Error, Result},
    input,
};
use fs2::FileExt;
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};
use std::{
    collections::BTreeMap,
    fs::{self, OpenOptions},
    io::Write,
    path::{Path, PathBuf},
};
use url::Url;
#[derive(Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Config {
    pub version: u32,
    pub current_context: Option<String>,
    #[serde(default)]
    pub contexts: BTreeMap<String, Context>,
}
#[derive(Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Context {
    pub endpoint: String,
    pub account: String,
    pub contract_profile: Option<String>,
    pub namespace: Option<String>,
    pub repository: Option<String>,
    #[serde(rename = "ref")]
    pub git_ref: Option<String>,
    pub git_endpoint: Option<String>,
    pub git_auth: Option<GitAuth>,
    #[serde(default)]
    pub allow_loopback_http: bool,
    pub accounts: BTreeMap<String, Account>,
}
#[derive(Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Account {
    pub issuer: String,
    #[serde(default = "store_default")]
    pub credential_source: String,
}
fn store_default() -> String {
    "os-store".into()
}
#[derive(Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct GitAuth {
    pub mechanism: String,
    pub account: String,
    pub issuer: String,
    pub credential_file: Option<PathBuf>,
}
#[derive(Clone)]
pub struct Effective {
    pub name: String,
    pub context: Context,
    pub sources: BTreeMap<String, String>,
}
pub fn path(c: &Cli) -> Result<PathBuf> {
    if let Some(p) = &c.config {
        if p.as_os_str().is_empty() {
            return Err(Error::usage("Config path cannot be empty"));
        }
        return Ok(p.clone());
    }
    let home =
        std::env::var_os("HOME").ok_or_else(|| Error::usage("HOME or --config is required"))?;
    #[cfg(target_os = "macos")]
    let p = PathBuf::from(home).join("Library/Application Support");
    #[cfg(not(target_os = "macos"))]
    let p = std::env::var_os("XDG_CONFIG_HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(home).join(".config"));
    Ok(p.join("gitstore/config.toml"))
}
pub fn load(p: &Path) -> Result<Config> {
    let s = input::bounded(
        std::fs::File::open(p).map_err(|_| Error::usage("Cannot open configuration file"))?,
    )?;
    let cfg: Config = toml::from_str(&s)
        .map_err(|_| Error::usage("Invalid config: check version, fields and duplicate keys"))?;
    cfg.validate()?;
    Ok(cfg)
}
pub fn normalized(s: &str, allow_http: bool) -> Result<String> {
    let mut u =
        Url::parse(s).map_err(|_| Error::usage("Endpoint must be an absolute HTTP(S) URL"))?;
    if !u.username().is_empty()
        || u.password().is_some()
        || u.query().is_some()
        || u.fragment().is_some()
        || u.host_str().is_none()
    {
        return Err(Error::usage(
            "Endpoint cannot contain credentials, query or fragment",
        ));
    }
    let loopback = u
        .host_str()
        .is_some_and(|h| h == "localhost" || h == "127.0.0.1" || h == "[::1]");
    if u.scheme() != "https" && !(u.scheme() == "http" && allow_http && loopback) {
        return Err(Error::usage(
            "HTTPS is required except explicitly enabled loopback HTTP",
        ));
    }
    let p = u.path().trim_end_matches('/').to_string();
    u.set_path(&p);
    Ok(u.as_str().trim_end_matches('/').into())
}
impl Config {
    pub fn validate(&self) -> Result<()> {
        if self.version != 1 {
            return Err(Error::usage("Unsupported configuration version"));
        }
        if self
            .current_context
            .as_ref()
            .is_some_and(|n| !self.contexts.contains_key(n))
        {
            return Err(Error::usage("Selected context does not exist"));
        }
        for (name, c) in &self.contexts {
            if name.is_empty() || c.account.is_empty() || !c.accounts.contains_key(&c.account) {
                return Err(Error::usage(
                    "Context name/account must be nonempty and reference a saved account",
                ));
            }
            normalized(&c.endpoint, c.allow_loopback_http)?;
            if let Some(s) = &c.git_endpoint {
                normalized(s, c.allow_loopback_http)?;
            }
            for (name, a) in &c.accounts {
                if name.is_empty() || a.issuer.is_empty() || a.credential_source != "os-store" {
                    return Err(Error::usage(
                        "Account name/issuer must be nonempty and credential_source must be os-store",
                    ));
                }
            }
            for s in [&c.namespace, &c.repository, &c.git_ref, &c.contract_profile]
                .into_iter()
                .flatten()
            {
                if s.is_empty() {
                    return Err(Error::usage("Configured selectors cannot be empty"));
                }
            }
            if let Some(a) = &c.git_auth
                && (!matches!(a.mechanism.as_str(), "basic" | "bearer")
                    || a.account.is_empty()
                    || a.issuer.is_empty()
                    || c.git_endpoint.is_none())
            {
                return Err(Error::usage(
                    "Git authentication needs an explicit endpoint, mechanism, account and issuer",
                ));
            }
        }
        Ok(())
    }
    pub fn effective(&self, cli: &Cli) -> Result<Effective> {
        let name = cli
            .context
            .as_ref()
            .or(self.current_context.as_ref())
            .ok_or_else(|| Error::usage("Select a context with --context or context use"))?;
        let mut context = self
            .contexts
            .get(name)
            .cloned()
            .ok_or_else(|| Error::usage("Context does not exist"))?;
        let mut selected_name = name.clone();
        let mut sources = BTreeMap::new();
        for k in ["endpoint", "account", "namespace", "repository", "ref"] {
            sources.insert(k.into(), "context".into());
        }
        if let Some(endpoint) = &cli.endpoint {
            let endpoint = normalized(endpoint, context.allow_loopback_http)?;
            if endpoint != normalized(&context.endpoint, context.allow_loopback_http)? {
                let matches: Vec<_> = self
                    .contexts
                    .iter()
                    .filter(|(_, c)| {
                        normalized(&c.endpoint, c.allow_loopback_http).ok().as_ref()
                            == Some(&endpoint)
                    })
                    .collect();
                if matches.len() == 1 {
                    context = matches[0].1.clone();
                    selected_name = matches[0].0.clone();
                } else if matches.is_empty()
                    && (cli.credential_file.is_some() || cli.credential_stdin)
                    && matches!(cli.command, crate::cli::Command::Api { .. })
                {
                    context.endpoint = endpoint.clone();
                    context.git_endpoint = None;
                    context.git_auth = None;
                    context.contract_profile = None;
                } else {
                    return Err(Error::usage(
                        "Endpoint override needs one matching saved installation; direct GraphQL may use protected explicit credentials",
                    ));
                }
            }
            context.endpoint = endpoint;
        }
        for (key, flag) in [
            ("endpoint", &cli.endpoint),
            ("account", &cli.account),
            ("namespace", &cli.namespace),
            ("repository", &cli.repository),
            ("ref", &cli.git_ref),
        ] {
            if flag.is_some() {
                sources.insert(key.into(), source_for(key).into());
            }
        }
        if let Some(a) = &cli.account {
            context.account = a.clone();
        }
        if !context.accounts.contains_key(&context.account) {
            return Err(Error::usage(
                "Account does not belong to selected installation",
            ));
        }
        if let Some(n) = &cli.namespace {
            context.namespace = Some(n.clone())
        }
        if let Some(r) = &cli.repository {
            context.repository = Some(r.clone())
        }
        if let Some(r) = &cli.git_ref {
            context.git_ref = Some(r.clone())
        }
        context.endpoint = normalized(&context.endpoint, context.allow_loopback_http)?;
        Ok(Effective {
            name: selected_name,
            context,
            sources,
        })
    }
}
fn source_for(key: &str) -> &'static str {
    let flag = if key == "ref" {
        "--ref".into()
    } else {
        format!("--{key}")
    };
    if std::env::args().any(|a| a == flag || a.starts_with(&(flag.clone() + "="))) {
        "flag"
    } else {
        "environment"
    }
}
impl Effective {
    pub fn summary(&self) -> Value {
        json!({"context":self.name,"endpoint":self.context.endpoint,"account":self.context.account,"namespace":self.context.namespace,"repository":self.context.repository,"ref":self.context.git_ref,"sources":self.sources})
    }
    pub fn namespace(&self) -> Result<&str> {
        self.context
            .namespace
            .as_deref()
            .filter(|s| !s.is_empty())
            .ok_or_else(|| Error::usage("Select a namespace"))
    }
    pub fn profile(&self) -> Result<()> {
        if self.context.contract_profile.as_deref() != Some("m0-2026-10-27") {
            return Err(Error::unsupported(
                "explicit compatible contract_profile required",
            ));
        }
        Ok(())
    }
}
pub fn mutate(p: &Path, update: impl FnOnce(&mut Config) -> Result<()>) -> Result<()> {
    mutate_with_directory_sync(p, update, fs::File::sync_all)
}
fn mutate_with_directory_sync(
    p: &Path,
    update: impl FnOnce(&mut Config) -> Result<()>,
    sync_directory: impl FnOnce(&fs::File) -> std::io::Result<()>,
) -> Result<()> {
    let parent = p
        .parent()
        .filter(|p| !p.as_os_str().is_empty())
        .unwrap_or(Path::new("."));
    let lock_path = p.with_extension("toml.lock");
    let mut opts = OpenOptions::new();
    opts.read(true).write(true).create(true).truncate(false);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        opts.mode(0o600).custom_flags(libc::O_NOFOLLOW);
    }
    let lock = opts
        .open(lock_path)
        .map_err(|_| Error::usage("Cannot open config selection lock"))?;
    lock.lock_exclusive()
        .map_err(|_| Error::usage("Cannot lock configuration"))?;
    let mut cfg = load(p)?;
    update(&mut cfg)?;
    cfg.validate()?;
    let s =
        toml::to_string_pretty(&cfg).map_err(|_| Error::usage("Cannot encode configuration"))?;
    let mut tmp = tempfile::NamedTempFile::new_in(parent)
        .map_err(|_| Error::usage("Cannot prepare atomic configuration update"))?;
    tmp.write_all(s.as_bytes())
        .and_then(|_| tmp.as_file().sync_all())
        .map_err(|_| Error::usage("Cannot persist configuration update"))?;
    // Opening the parent must succeed before replacing the selection.
    let directory = fs::File::open(parent)
        .map_err(|_| Error::usage("Cannot open config directory for persistence"))?;
    tmp.persist(p)
        .map_err(|_| Error::usage("Cannot replace configuration"))?;
    if let Err(error) = sync_directory(&directory)
        && !directory_sync_unsupported(&error)
    {
        let mut error = Error::new(
            1,
            "CONFIG_DURABILITY",
            "Configuration changed, but directory sync failed; crash durability is unconfirmed",
        );
        error.stage = "persistence";
        return Err(error);
    }
    Ok(())
}
fn directory_sync_unsupported(error: &std::io::Error) -> bool {
    if error.kind() == std::io::ErrorKind::Unsupported {
        return true;
    }
    #[cfg(unix)]
    if let Some(code) = error.raw_os_error() {
        // Directory fsync is not implemented on every supported filesystem.
        return code == libc::EINVAL || code == libc::ENOTSUP || code == libc::EOPNOTSUPP;
    }
    false
}

#[cfg(test)]
mod persistence_tests {
    use super::*;

    fn selection_with_sync(result: std::io::Result<()>) -> Result<()> {
        let directory = tempfile::tempdir().unwrap();
        let path = directory.path().join("config.toml");
        fs::write(
            &path,
            r#"version = 1
current_context = "one"
[contexts.one]
endpoint = "https://example.test/graphql"
account = "alice"
[contexts.one.accounts.alice]
issuer = "test"
[contexts.one.accounts.bob]
issuer = "test"
"#,
        )
        .unwrap();
        let changed = mutate_with_directory_sync(
            &path,
            |config| {
                config.contexts.get_mut("one").unwrap().account = "bob".into();
                Ok(())
            },
            |_| result,
        );
        assert_eq!(load(&path).unwrap().contexts["one"].account, "bob");
        changed
    }

    #[test]
    fn unsupported_directory_sync_does_not_report_selection_failure() {
        selection_with_sync(Err(std::io::ErrorKind::Unsupported.into())).unwrap();
        #[cfg(unix)]
        for code in [libc::EINVAL, libc::ENOTSUP, libc::EOPNOTSUPP] {
            selection_with_sync(Err(std::io::Error::from_raw_os_error(code))).unwrap();
        }
    }

    #[test]
    fn real_directory_sync_failure_reports_changed_selection_and_uncertain_durability() {
        #[cfg(unix)]
        for code in [libc::EIO, libc::EACCES, libc::EPERM] {
            let error =
                selection_with_sync(Err(std::io::Error::from_raw_os_error(code))).unwrap_err();
            assert_eq!(error.exit, 1);
            assert_eq!(error.code, "CONFIG_DURABILITY");
            assert_eq!(error.stage, "persistence");
            assert!(error.message.contains("Configuration changed"));
        }
    }
}
