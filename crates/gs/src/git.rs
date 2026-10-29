#[path = "git_http.rs"]
mod http;
// In-process Git operations. No Git executable or external credential helper is used.
use crate::{
    cli::{Cli, RepositoryCommand},
    config::{self, Effective},
    credentials::{Native, Secret, Store},
    error::{Error, Result},
    input,
};
use git2::{
    ErrorCode, FetchOptions, Oid, PushOptions, RemoteCallbacks, RemoteRedirect, Repository,
};
use serde_json::{Value, json};
use std::sync::Arc;
use std::{
    cell::RefCell,
    io::{self, Write},
    path::{Path, PathBuf},
    rc::Rc,
    time::{Duration, Instant},
};
use url::Url;
use zeroize::Zeroizing;

fn failure(e: git2::Error) -> Error {
    if e.class() == git2::ErrorClass::Http {
        match e.message().strip_prefix("unexpected http status code: ") {
            Some("401" | "403" | "407") => {
                return Error::auth("Git authentication or authorization failed");
            }
            Some("404") => return Error::new(4, "NOT_FOUND", "Git target not found"),
            _ => {}
        }
    }
    match e.code() {
        ErrorCode::Auth => Error::auth("Git authentication or authorization failed"),
        ErrorCode::NotFound => Error::new(4, "NOT_FOUND", "Git target not found"),
        ErrorCode::NotFastForward | ErrorCode::Modified | ErrorCode::Locked => {
            Error::new(5, "CONFLICT", "Git ref update conflict")
        }
        _ => Error::remote("Git library operation failed"),
    }
}
fn lib<T>(r: std::result::Result<T, git2::Error>) -> Result<T> {
    r.map_err(failure)
}
fn segment(s: &str) -> Result<()> {
    if s.is_empty() || matches!(s, "." | "..") || s.contains(['/', '\\', '\0']) {
        Err(Error::usage("Invalid repository/namespace path segment"))
    } else {
        Ok(())
    }
}
fn root(e: &Effective) -> Result<String> {
    config::normalized(
        e.context.git_endpoint.as_deref().ok_or_else(|| {
            Error::usage("Configure the public Git endpoint independently of GraphQL")
        })?,
        e.context.allow_loopback_http,
    )
}
pub fn repository_url(e: &Effective, repository: &str) -> Result<String> {
    segment(repository)?;
    let namespace = e.namespace()?;
    segment(namespace)?;
    let mut u = Url::parse(&root(e)?).map_err(|_| Error::usage("Invalid Git endpoint"))?;
    u.path_segments_mut()
        .map_err(|_| Error::usage("Git endpoint cannot be a base URL"))?
        .pop_if_empty()
        .push(namespace)
        .push(&format!("{repository}.git"));
    Ok(u.into())
}
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Target {
    pub url: String,
    pub namespace: String,
    pub repository: String,
}
pub fn target(e: &Effective, url: &str) -> Result<Target> {
    let normalized = config::normalized(url, e.context.allow_loopback_http)?;
    let base = root(e)?;
    let prefix = base + "/";
    let suffix = normalized
        .strip_prefix(&prefix)
        .ok_or_else(|| Error::usage("Git remote does not match selected installation endpoint"))?;
    let parts: Vec<_> = suffix.split('/').collect();
    if parts.len() != 2 {
        return Err(Error::usage(
            "Git remote must identify exactly namespace/repository",
        ));
    }
    let namespace = percent_encoding::percent_decode_str(parts[0])
        .decode_utf8()
        .map_err(|_| Error::usage("Invalid namespace encoding"))?
        .into_owned();
    let repository =
        percent_encoding::percent_decode_str(parts[1].strip_suffix(".git").unwrap_or(parts[1]))
            .decode_utf8()
            .map_err(|_| Error::usage("Invalid repository encoding"))?
            .into_owned();
    segment(&namespace)?;
    segment(&repository)?;
    Ok(Target {
        url: normalized,
        namespace,
        repository,
    })
}
fn check_explicit(cli: &Cli, e: &Effective, positional: Option<&str>, t: &Target) -> Result<()> {
    if let (Some(a), Some(b)) = (positional, cli.repository.as_deref())
        && a != b
    {
        return Err(Error::usage(
            "Repository argument conflicts with --repository",
        ));
    }
    if positional
        .or(cli.repository.as_deref())
        .is_some_and(|r| r != t.repository)
    {
        return Err(Error::usage("Remote does not match explicit repository"));
    }
    if cli.namespace.as_ref().is_some_and(|n| *n != t.namespace) {
        return Err(Error::usage("Remote does not match explicit namespace"));
    }
    if positional.or(cli.repository.as_deref()).is_some()
        && e.context
            .namespace
            .as_ref()
            .is_some_and(|n| n != &t.namespace)
    {
        return Err(Error::usage(
            "Explicit repository conflicts with selected namespace",
        ));
    }
    Ok(())
}
/// Replace layered configuration with whitelisted local values, never ambient auth,
/// helpers, URL rewrites, TLS overrides, filters or hooks. Keep its backend alive.
fn isolate(repo: &Repository) -> Result<tempfile::NamedTempFile> {
    let tmp = tempfile::NamedTempFile::new()
        .map_err(|_| Error::remote("Cannot create isolated Git configuration"))?;
    let mut safe = lib(git2::Config::open(tmp.path()))?;
    let local = lib(git2::Config::open(&repo.commondir().join("config")))?;
    let mut entries = lib(local.entries(None))?;
    let mut target_keys = std::collections::HashSet::new();
    while let Some(entry) = entries.next() {
        let entry = lib(entry)?;
        if let (Ok(k), Ok(v)) = (entry.name(), entry.value()) {
            if k.starts_with("url.") {
                return Err(Error::unsupported(
                    "Git URL rewriting is not supported for isolated targets",
                ));
            }
            let allowed = (k.starts_with("remote.")
                && (k.ends_with(".url") || k.ends_with(".pushurl") || k.ends_with(".fetch")))
                || (k.starts_with("branch.") && (k.ends_with(".remote") || k.ends_with(".merge")))
                || matches!(
                    k,
                    "core.bare"
                        | "core.repositoryformatversion"
                        | "core.filemode"
                        | "core.ignorecase"
                        | "core.logallrefupdates"
                );
            if k.starts_with("extensions.") {
                return Err(Error::unsupported(
                    "repository extension requires compatibility validation",
                ));
            }
            if allowed {
                if (k.ends_with(".url") || k.ends_with(".pushurl") || k.ends_with(".merge"))
                    && !target_keys.insert(k.to_owned())
                {
                    return Err(Error::usage(
                        "Ambiguous multi-valued Git target configuration",
                    ));
                }
                lib(safe.set_str(k, v))?;
            }
        }
    }
    lib(safe.set_bool("http.followRedirects", false))?;
    lib(repo.set_config(&safe))?;
    Ok(tmp)
}
enum Auth {
    Basic { username: String, password: Secret },
    Bearer { token: Secret },
}
fn material(e: &Effective) -> Result<Auth> {
    let a = e.context.git_auth.as_ref().ok_or_else(|| {
        Error::usage("Configure an explicit Git authentication mechanism and identity")
    })?;
    let raw = if let Some(p) = &a.credential_file {
        Zeroizing::new(input::protected(p)?)
    } else {
        let key = json!([
            e.context.endpoint,
            a.issuer,
            a.account,
            "git",
            root(e)?,
            a.mechanism
        ])
        .to_string();
        Zeroizing::new(
            Native
                .read(&key)?
                .ok_or_else(|| Error::auth("No credential for the selected Git identity"))?
                .expose()
                .to_owned(),
        )
    };
    let v = input::json(&raw).map_err(|_| Error::auth("Invalid protected Git credential JSON"))?;
    match a.mechanism.as_str() {
        "basic" => {
            if v.as_object().is_none_or(|m| m.len() != 2) {
                return Err(Error::auth("Basic material needs username/password only"));
            }
            let username = v["username"]
                .as_str()
                .filter(|s| !s.is_empty() && !s.contains(['\r', '\n', '\0']))
                .ok_or_else(|| Error::auth("Missing Git username"))?
                .into();
            let password = Secret::new(
                v["password"]
                    .as_str()
                    .ok_or_else(|| Error::auth("Missing Git password"))?
                    .into(),
            )?;
            Ok(Auth::Basic { username, password })
        }
        "bearer" => {
            if v.as_object().is_none_or(|m| m.len() != 1) {
                return Err(Error::auth("Bearer material needs token only"));
            }
            Ok(Auth::Bearer {
                token: Secret::new(
                    v["token"]
                        .as_str()
                        .ok_or_else(|| Error::auth("Missing Git Bearer token"))?
                        .into(),
                )?,
            })
        }
        _ => Err(Error::unsupported("configured Git mechanism")),
    }
}
struct Transport {
    auth: Arc<Auth>,
    network: http::Network,
    deadline: Instant,
    rejected: Rc<RefCell<Option<Error>>>,
    stale: Rc<RefCell<bool>>,
}
impl Transport {
    fn new(e: &Effective, timeout: Duration, allowed: Vec<String>) -> Result<Self> {
        let deadline = Instant::now() + timeout;
        let auth = Arc::new(material(e)?);
        let network = http::Network::install(auth.clone(), allowed, deadline)?;
        Ok(Self {
            auth,
            network,
            deadline,
            rejected: Rc::new(RefCell::new(None)),
            stale: Rc::new(RefCell::new(false)),
        })
    }
    fn callbacks<'a>(&'a self, expected: Option<Oid>) -> RemoteCallbacks<'a> {
        let mut cb = RemoteCallbacks::new();
        let deadline = self.deadline;
        cb.transfer_progress(move |_| Instant::now() < deadline);
        let secrets = match self.auth.as_ref() {
            Auth::Basic { password, .. } => vec![password.expose().as_bytes()],
            Auth::Bearer { token } => vec![token.expose().as_bytes()],
        };
        let mut redactor = ProgressRedactor {
            secrets,
            pending: Vec::new(),
        };
        cb.sideband_progress(move |data| {
            if Instant::now() >= deadline {
                return false;
            }
            let safe = redactor.feed(data);
            let _ = io::stderr().lock().write_all(&safe);
            let _ = io::stderr().lock().flush();
            true
        });
        let reject = self.rejected.clone();
        cb.push_update_reference(move |_, status| {
            if let Some(status) = status {
                let mut error = if status.contains("non-fast-forward") || status.contains("stale") {
                    Error::new(
                        5,
                        "CONFLICT",
                        "Server rejected the ref update as conflicting",
                    )
                } else {
                    Error::remote("Server rejected the ref update")
                };
                error.submission = Some("not-submitted".into());
                *reject.borrow_mut() = Some(error);
            }
            Ok(())
        });
        let stale = self.stale.clone();
        cb.push_negotiation(move |updates| {
            if Instant::now() >= deadline {
                return Err(git2::Error::from_str("deadline"));
            }
            if let Some(expected) = expected
                && updates.iter().any(|u| u.src() != expected)
            {
                *stale.borrow_mut() = true;
                return Err(git2::Error::new(
                    ErrorCode::NotFastForward,
                    git2::ErrorClass::Callback,
                    "remote advanced",
                ));
            }
            Ok(())
        });
        cb
    }
    fn fetch<'a>(&'a self, _url: &'a str) -> FetchOptions<'a> {
        let mut f = FetchOptions::new();
        f.remote_callbacks(self.callbacks(None))
            .follow_redirects(RemoteRedirect::None)
            .download_tags(git2::AutotagOption::None);
        f
    }
    fn timeout(&self) -> Result<()> {
        if Instant::now() >= self.deadline {
            Err(Error::deadline())
        } else {
            Ok(())
        }
    }
    fn result<T>(&self, result: std::result::Result<T, git2::Error>, push: bool) -> Result<T> {
        match result {
            Ok(x) => {
                if *self.stale.borrow() {
                    Err(Error::new(5, "CONFLICT", "Remote advanced after fetch"))
                } else if let Some(error) = self.rejected.borrow_mut().take() {
                    Err(error)
                } else {
                    Ok(x)
                }
            }
            Err(e) => {
                if let Some(mut error) = self.network.error() {
                    if push {
                        error = error.uncertain();
                    }
                    return Err(error);
                }
                let timeout = Instant::now() >= self.deadline || e.code() == ErrorCode::Timeout;
                let mut error = if timeout {
                    Error::deadline()
                } else {
                    failure(e)
                };
                if *self.stale.borrow() {
                    error = Error::new(5, "CONFLICT", "Remote advanced after fetch")
                };
                if push && error.exit != 5 {
                    error = error.uncertain()
                }
                Err(error)
            }
        }
    }
}
pub fn clone_repository(
    e: &Effective,
    repository: &str,
    destination: &Path,
    remote: &str,
    timeout: Duration,
) -> Result<Value> {
    e.profile()?;
    if !git2::Remote::is_valid_name(remote) {
        return Err(Error::usage("Invalid remote name"));
    }
    if destination.exists()
        && (!destination.is_dir()
            || std::fs::read_dir(destination)
                .map_err(|_| Error::usage("Cannot inspect clone destination"))?
                .next()
                .is_some())
    {
        return Err(Error::usage("Clone destination must be absent or empty"));
    }
    let url = repository_url(e, repository)?;
    let transport = Transport::new(e, timeout, vec![url.clone()])?;
    let repo = lib(Repository::init_opts(destination, &{
        let mut o = git2::RepositoryInitOptions::new();
        o.external_template(false);
        o
    }))?;
    // Persist only public remote/ref data to local config. isolate() removes all ambient auth/filters.
    {
        lib(repo.remote(remote, &url))?;
    }
    let _safe = isolate(&repo)?;
    transport.timeout()?;
    let mut r = lib(repo.find_remote(remote))?;
    let mut options = transport.fetch(&url);
    transport.result(r.fetch(&[] as &[&str], Some(&mut options), None), false)?;
    let default = r
        .default_branch()
        .ok()
        .and_then(|b| b.as_str().ok().map(str::to_owned));
    let mut branch = None;
    if let Some(full) = default.filter(|s| s.starts_with("refs/heads/")) {
        let name = &full[11..];
        let tracking = format!("refs/remotes/{remote}/{name}");
        let id = lib(repo.find_reference(&tracking).and_then(|r| {
            r.target()
                .ok_or_else(|| git2::Error::from_str("missing target"))
        }))?;
        lib(repo.reference(&full, id, false, "clone"))?;
        lib(repo.set_head(&full))?;
        lib(repo.checkout_head(Some(
            git2::build::CheckoutBuilder::new()
                .safe()
                .disable_filters(true),
        )))?;
        let mut disk = lib(git2::Config::open(&repo.commondir().join("config")))?;
        lib(disk.set_str(&format!("branch.{name}.remote"), remote))?;
        lib(disk.set_str(&format!("branch.{name}.merge"), &full))?;
        branch = Some(name.to_string());
    }
    Ok(
        json!({"destination":destination,"remote":remote,"url":url,"branch":branch,"trackingBranch":branch.as_ref().map(|b|format!("{remote}/{b}"))}),
    )
}
pub fn sync(
    cli: &Cli,
    e: &Effective,
    explicit: Option<&str>,
    remote_name: Option<&str>,
    cwd: &Path,
) -> Result<Value> {
    e.profile()?;
    let repo = lib(Repository::discover(cwd))?;
    if repo.is_bare() {
        return Err(Error::usage("Sync requires a worktree"));
    }
    let _safe = isolate(&repo)?;
    if repo.state() != git2::RepositoryState::Clean {
        return Err(Error::usage(
            "Finish the active Git operation before syncing",
        ));
    }
    let mut status = git2::StatusOptions::new();
    status.include_untracked(true).recurse_untracked_dirs(true);
    if !lib(repo.statuses(Some(&mut status)))?.is_empty() {
        return Err(Error::usage("Sync requires a clean worktree"));
    }
    let names = lib(repo.remotes())?;
    let mut candidates = vec![];
    for value in names.iter() {
        let name = lib(value)?.ok_or_else(|| Error::usage("Invalid remote name"))?;
        if remote_name.is_some_and(|r| r != name) {
            continue;
        }
        let r = lib(repo.find_remote(name))?;
        let url = lib(r.url())?;
        let t = match target(e, url) {
            Ok(t) => t,
            Err(_) => {
                if remote_name.is_some() {
                    return Err(Error::usage("Selected remote does not match installation"));
                }
                continue;
            }
        };
        if check_explicit(cli, e, explicit, &t).is_err() {
            if remote_name.is_some() {
                check_explicit(cli, e, explicit, &t)?
            }
            continue;
        }
        let push = lib(r.pushurl())?.unwrap_or(url);
        let pt = target(e, push)?;
        if t.namespace != pt.namespace || t.repository != pt.repository {
            return Err(Error::usage("Fetch and push target different repositories"));
        }
        candidates.push((name.to_owned(), t, push.to_owned()));
    }
    if candidates.len() != 1 {
        return Err(Error::usage(
            "Select one unambiguous GitStore repository/remote",
        ));
    }
    let (name, target, push_url) = candidates.remove(0);
    let head = lib(repo.head())?;
    let local = lib(head.peel_to_commit())?.id();
    let full_ref = if let Some(value) = &e.context.git_ref {
        if value.starts_with("refs/") {
            value.clone()
        } else {
            format!("refs/heads/{value}")
        }
    } else {
        if !head.is_branch() {
            return Err(Error::usage("Detached HEAD requires explicit --ref"));
        }
        let branch = lib(head.shorthand())?;
        let cfg = lib(repo.config())?;
        let upstream_remote = lib(cfg.get_string(&format!("branch.{branch}.remote")))?;
        if upstream_remote != name {
            return Err(Error::usage(
                "Branch upstream does not match selected remote; use explicit --ref",
            ));
        }
        lib(cfg.get_string(&format!("branch.{branch}.merge")))?
    };
    if !full_ref.starts_with("refs/heads/") || !git2::Reference::is_valid_name(&full_ref) {
        return Err(Error::usage("Sync target must be a valid branch ref"));
    }
    let tracking = format!("refs/remotes/{}/{}", name, &full_ref[11..]);
    let mut remote = lib(repo.remote_anonymous(&target.url))?;
    let transport = Transport::new(e, cli.timeout, vec![target.url.clone(), push_url.clone()])?;
    transport.timeout()?;
    let mut fetch = transport.fetch(&target.url);
    let refspec = format!("+{full_ref}:{tracking}");
    transport.result(remote.fetch(&[&refspec], Some(&mut fetch), None), false)?;
    let old = lib(repo.find_reference(&tracking))?
        .target()
        .ok_or_else(|| Error::remote("Missing fetched commit"))?;
    if old == local {
        return Ok(
            json!({"submission":"not-submitted","reason":"no-new-commit","revision":local.to_string(),"remote":name,"ref":full_ref}),
        );
    }
    if !lib(repo.graph_descendant_of(local, old))? {
        return Err(Error::new(
            5,
            "CONFLICT",
            "Local branch is behind or divergent; use explicit Git action",
        ));
    }
    transport.timeout()?;
    let mut pushing = lib(repo.remote_anonymous(&push_url))?;
    let mut options = PushOptions::new();
    options
        .remote_callbacks(transport.callbacks(Some(old)))
        .follow_redirects(RemoteRedirect::None);
    let push_refspec = format!("{local}:{full_ref}");
    transport
        .result(pushing.push(&[&push_refspec], Some(&mut options)), true)
        .map_err(|mut err| {
            err.revision = Some(local.to_string());
            err
        })?;
    Ok(
        json!({"submission":"accepted","revision":local.to_string(),"remote":name,"ref":full_ref,"admission":"unverified","readiness":"unverified"}),
    )
}
pub fn dispatch(cli: &Cli, e: &Effective, command: &RepositoryCommand) -> Result<Value> {
    match command {
        RepositoryCommand::Clone {
            repository,
            directory,
            remote,
        } => {
            if cli.repository.as_ref().is_some_and(|r| r != repository) {
                return Err(Error::usage(
                    "Repository argument conflicts with --repository",
                ));
            }
            let path = directory
                .clone()
                .unwrap_or_else(|| PathBuf::from(repository));
            clone_repository(e, repository, &path, remote, cli.timeout)
        }
        RepositoryCommand::Sync { repository, remote } => sync(
            cli,
            e,
            repository.as_deref(),
            remote.as_deref(),
            &std::env::current_dir()
                .map_err(|_| Error::usage("Cannot resolve working directory"))?,
        ),
    }
}

struct ProgressRedactor<'a> {
    secrets: Vec<&'a [u8]>,
    pending: Vec<u8>,
}
impl ProgressRedactor<'_> {
    fn feed(&mut self, data: &[u8]) -> Vec<u8> {
        self.pending.extend_from_slice(data);
        let mut output = Vec::new();
        let mut offset = 0;
        while offset < self.pending.len() {
            let tail = &self.pending[offset..];
            if let Some(secret) = self
                .secrets
                .iter()
                .find(|s| !s.is_empty() && tail.starts_with(s))
            {
                output.extend_from_slice(b"[REDACTED]");
                offset += secret.len();
            } else if self.secrets.iter().any(|s| s.starts_with(tail)) {
                break;
            } else {
                let b = self.pending[offset];
                if b == 0x1b {
                    output.extend_from_slice(b"[ESC]");
                } else {
                    output.push(b);
                }
                offset += 1;
            }
        }
        self.pending.drain(..offset);
        output
    }
}
#[cfg(test)]
mod progress_tests {
    use super::*;
    #[test]
    fn redacts_secrets_across_callback_boundaries() {
        let mut r = ProgressRedactor {
            secrets: vec![b"password"],
            pending: vec![],
        };
        let mut result = r.feed(b"message pass");
        result.extend(r.feed(b"word!\n"));
        assert_eq!(result, b"message [REDACTED]!\n");
    }
}
