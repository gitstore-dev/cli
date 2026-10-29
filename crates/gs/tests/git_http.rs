#[path = "support/git_http.rs"]
mod git_http;
mod support;
use git_http::*;
use std::{path::Path, sync::atomic::Ordering};
use support::*;
fn configure(h: &Harness, endpoint: &str, mechanism: &str) {
    h.configure("http://127.0.0.1:1/graphql");
    let p = h.write(
        "git-secret",
        if mechanism == "basic" {
            r#"{"username":"user","password":"password"}"#
        } else {
            r#"{"token":"synthetic-git-token"}"#
        },
    );
    let mut cfg = std::fs::read_to_string(h.config()).unwrap();
    cfg = cfg.replace(
        "namespace = \"shop\"",
        &format!("namespace = \"shop\"\ngit_endpoint = \"{endpoint}\""),
    );
    cfg += &format!(
        "\n[contexts.one.git_auth]\nmechanism = \"{mechanism}\"\naccount = \"git-user\"\nissuer = \"fixture\"\ncredential_file = '{}'\n",
        p.display()
    );
    std::fs::write(h.config(), cfg).unwrap();
}
fn setup(auth: Auth) -> (Harness, HttpGit) {
    let h = Harness::new();
    let source = seed(&h.dir.path().join("source"));
    commit(&source, "initial\n");
    let bare = h.dir.path().join("server.git");
    git2::build::RepoBuilder::new()
        .bare(true)
        .clone(source.path().to_str().unwrap(), &bare)
        .unwrap();
    let http = HttpGit::new(&bare, auth);
    (h, http)
}
#[test]
fn clone_and_fast_forward_sync_without_git_executable() {
    let (h, http) = setup(Auth::Basic);
    configure(&h, &http.endpoint, "basic");
    let dest = h.dir.path().join("catalog");
    let o = h.run(&[
        "repository",
        "clone",
        "catalog",
        dest.to_str().unwrap(),
        "-o",
        "json",
    ]);
    assert!(o.status.success(), "{}", stderr(&o));
    let repo = git2::Repository::open(&dest).unwrap();
    assert_eq!(repo.remotes().unwrap().len(), 1);
    assert!(repo.find_remote("gitstore").is_ok());
    assert_eq!(
        repo.config()
            .unwrap()
            .get_string("branch.main.remote")
            .unwrap(),
        "gitstore"
    );
    let o = h
        .cmd()
        .current_dir(&dest)
        .args(["repository", "sync", "-o", "json"])
        .output()
        .unwrap();
    assert!(o.status.success(), "{}", stderr(&o));
    assert!(stdout(&o).contains("no-new-commit"));
    let new = commit(&repo, "changed\n");
    let before = repo.head().unwrap().target();
    let sub = dest.join("nested");
    std::fs::create_dir(&sub).unwrap();
    let o = h
        .cmd()
        .current_dir(&sub)
        .args(["repository", "sync", "-o", "json"])
        .output()
        .unwrap();
    assert!(o.status.success(), "{}", stderr(&o));
    assert!(stdout(&o).contains("unverified"));
    assert_eq!(repo.head().unwrap().target(), before);
    assert_eq!(
        git2::Repository::open_bare(&http.path)
            .unwrap()
            .head()
            .unwrap()
            .target(),
        Some(new)
    );
    assert!(
        http.calls
            .lock()
            .unwrap()
            .iter()
            .any(|s| s.ends_with("git-receive-pack"))
    );
}
#[test]
fn override_remote_bearer_and_ref_rejection() {
    let (h, http) = setup(Auth::Bearer);
    configure(&h, &http.endpoint, "bearer");
    let dest = h.dir.path().join("catalog");
    let o = h.run(&[
        "repository",
        "clone",
        "catalog",
        dest.to_str().unwrap(),
        "--remote",
        "origin",
    ]);
    assert!(o.status.success(), "{}", stderr(&o));
    let repo = git2::Repository::open(&dest).unwrap();
    assert!(repo.find_remote("gitstore").is_err());
    assert_eq!(
        repo.config()
            .unwrap()
            .get_string("branch.main.remote")
            .unwrap(),
        "origin"
    );
    commit(&repo, "new\n");
    http.reject_push.store(true, Ordering::Relaxed);
    let o = h
        .cmd()
        .current_dir(&dest)
        .args(["repository", "sync", "-o", "json"])
        .output()
        .unwrap();
    assert_eq!(o.status.code(), Some(8), "{}", stderr(&o));
}
#[test]
fn dirty_ambiguous_and_mismatched_push_fail_before_request() {
    let (h, http) = setup(Auth::None);
    configure(&h, &http.endpoint, "bearer");
    let dest = h.dir.path().join("catalog");
    assert!(
        h.run(&["repository", "clone", "catalog", dest.to_str().unwrap()])
            .status
            .success()
    );
    let repo = git2::Repository::open(&dest).unwrap();
    let count = http.calls.lock().unwrap().len();
    std::fs::write(dest.join("untracked"), "dirty").unwrap();
    let o = h
        .cmd()
        .current_dir(&dest)
        .args(["repository", "sync"])
        .output()
        .unwrap();
    assert_eq!(o.status.code(), Some(2));
    std::fs::remove_file(dest.join("untracked")).unwrap();
    repo.remote("other", &format!("{}/shop/catalog.git", http.endpoint))
        .unwrap();
    let o = h
        .cmd()
        .current_dir(&dest)
        .args(["repository", "sync"])
        .output()
        .unwrap();
    assert_eq!(o.status.code(), Some(2));
    repo.remote_delete("other").unwrap();
    repo.remote_set_pushurl("gitstore", Some("https://unrelated.test/shop/catalog.git"))
        .unwrap();
    let o = h
        .cmd()
        .current_dir(&dest)
        .args(["repository", "sync"])
        .output()
        .unwrap();
    assert_eq!(o.status.code(), Some(2));
    assert_eq!(http.calls.lock().unwrap().len(), count);
    assert!(!Path::new("/nonexistent/git").exists());
}
#[test]
fn empty_clone_does_not_invent_checked_out_branch() {
    let h = Harness::new();
    let bare = h.dir.path().join("empty.git");
    git2::Repository::init_bare(&bare).unwrap();
    let http = HttpGit::new(&bare, Auth::None);
    configure(&h, &http.endpoint, "bearer");
    let dest = h.dir.path().join("empty");
    let o = h.run(&[
        "repository",
        "clone",
        "catalog",
        dest.to_str().unwrap(),
        "-o",
        "json",
    ]);
    assert!(o.status.success(), "{}", stderr(&o));
    let value: serde_json::Value = serde_json::from_slice(&o.stdout).unwrap();
    assert!(value["branch"].is_null());
}

#[test]
fn behind_divergent_and_concurrent_advancement_preserve_local_history() {
    let (h, http) = setup(Auth::None);
    configure(&h, &http.endpoint, "bearer");
    let dest = h.dir.path().join("catalog");
    assert!(
        h.run(&["repository", "clone", "catalog", dest.to_str().unwrap()])
            .status
            .success()
    );
    let repo = git2::Repository::open(&dest).unwrap();
    let initial = repo.head().unwrap().target().unwrap();
    let local_new = commit(&repo, "local ahead\n");
    let server = git2::Repository::open_bare(&http.path).unwrap();
    // Seed the fixture's object database with a competing descendant using the library.
    let source = seed(&h.dir.path().join("competitor"));
    let blob = source.blob(b"other").unwrap();
    let mut builder = source.treebuilder(None).unwrap();
    builder.insert("other", blob, 0o100644).unwrap();
    let tree = source.find_tree(builder.write().unwrap()).unwrap();
    let sig = git2::Signature::now("Fixture", "f@example.test").unwrap();
    let unrelated = source
        .commit(Some("HEAD"), &sig, &sig, "unrelated", &tree, &[])
        .unwrap();
    let mut pack = source.packbuilder().unwrap();
    pack.insert_commit(unrelated).unwrap();
    let mut buf = git2::Buf::new();
    pack.write_buf(&mut buf).unwrap();
    let odb = server.odb().unwrap();
    let mut writer = odb.packwriter().unwrap();
    std::io::Write::write_all(&mut writer, &buf).unwrap();
    writer.commit().unwrap();
    *http.advance.lock().unwrap() = Some(unrelated);
    let o = h
        .cmd()
        .current_dir(&dest)
        .args(["repository", "sync"])
        .output()
        .unwrap();
    assert_eq!(o.status.code(), Some(5), "{}", stderr(&o));
    assert_eq!(repo.head().unwrap().target(), Some(local_new));
    // Once local is behind a known remote descendant, it must also fail without edits.
    server
        .reference("refs/heads/main", initial, true, "restore fixture")
        .unwrap();
    let mut transfer = repo.remote_anonymous(http.path.to_str().unwrap()).unwrap();
    transfer
        .push(&["refs/heads/main:refs/heads/main"], None)
        .unwrap();
    repo.reference("refs/heads/main", initial, true, "fixture behind")
        .unwrap();
    repo.checkout_head(Some(git2::build::CheckoutBuilder::new().force()))
        .unwrap();
    let o = h
        .cmd()
        .current_dir(&dest)
        .args(["repository", "sync"])
        .output()
        .unwrap();
    assert_eq!(o.status.code(), Some(5), "{}", stderr(&o));
    assert_eq!(repo.head().unwrap().target(), Some(initial));
}
#[test]
fn ambient_helpers_filters_and_headers_are_not_executed_or_forwarded() {
    let (h, http) = setup(Auth::Bearer);
    configure(&h, &http.endpoint, "bearer");
    let dest = h.dir.path().join("catalog");
    let o = h.run(&["repository", "clone", "catalog", dest.to_str().unwrap()]);
    assert!(o.status.success(), "{}", stderr(&o));
    let repo = git2::Repository::open(&dest).unwrap();
    let marker = h.dir.path().join("forbidden-helper");
    let command = format!("!touch {}", marker.display());
    let mut config = repo.config().unwrap();
    config.set_str("credential.helper", &command).unwrap();
    config
        .set_str(
            "http.extraHeader",
            "Authorization: Bearer wrong-ambient-token",
        )
        .unwrap();
    config.set_bool("http.sslVerify", false).unwrap();
    let o = h
        .cmd()
        .current_dir(&dest)
        .args(["repository", "sync", "-o", "json"])
        .output()
        .unwrap();
    assert!(o.status.success(), "{}", stderr(&o));
    assert!(!marker.exists());
    let disk = std::fs::read_to_string(repo.commondir().join("config")).unwrap();
    assert!(!disk.contains("synthetic-git-token"));
    assert!(
        !String::from_utf8(o.stderr)
            .unwrap()
            .contains("synthetic-git-token")
    );
}

#[test]
fn denied_redirected_and_stalled_transports_fail_with_precise_categories() {
    for (mode, code) in [(Mode::Deny(403), 3), (Mode::Redirect, 8), (Mode::Delay, 7)] {
        let (h, http) = setup(Auth::None);
        configure(&h, &http.endpoint, "bearer");
        *http.mode.lock().unwrap() = mode;
        let dest = h.dir.path().join("catalog");
        let o = h.run(&[
            "--timeout",
            "100ms",
            "repository",
            "clone",
            "catalog",
            dest.to_str().unwrap(),
            "-o",
            "json",
        ]);
        assert_eq!(o.status.code(), Some(code), "{}", stderr(&o));
        assert_eq!(http.calls.lock().unwrap().len(), 1);
    }
}
#[test]
fn lost_push_response_retains_revision_and_reports_uncertainty_without_retry() {
    let (h, http) = setup(Auth::None);
    configure(&h, &http.endpoint, "bearer");
    let dest = h.dir.path().join("catalog");
    assert!(
        h.run(&["repository", "clone", "catalog", dest.to_str().unwrap()])
            .status
            .success()
    );
    let repo = git2::Repository::open(&dest).unwrap();
    let revision = commit(&repo, "lost response\n");
    *http.mode.lock().unwrap() = Mode::LostResponse;
    let o = h
        .cmd()
        .current_dir(&dest)
        .args(["repository", "sync", "-o", "json"])
        .output()
        .unwrap();
    assert_eq!(o.status.code(), Some(8));
    let error: serde_json::Value = serde_json::from_slice(&o.stderr).unwrap();
    assert_eq!(error["submission"], "unknown");
    assert_eq!(error["revision"], revision.to_string());
    assert_eq!(
        git2::Repository::open_bare(&http.path)
            .unwrap()
            .head()
            .unwrap()
            .target(),
        Some(revision)
    );
    assert_eq!(
        http.calls
            .lock()
            .unwrap()
            .iter()
            .filter(|p| p.ends_with("/git-receive-pack"))
            .count(),
        1
    );
}
#[test]
fn detached_explicit_ref_is_allowed_but_implicit_ref_is_rejected() {
    let (h, http) = setup(Auth::None);
    configure(&h, &http.endpoint, "bearer");
    let dest = h.dir.path().join("catalog");
    assert!(
        h.run(&["repository", "clone", "catalog", dest.to_str().unwrap()])
            .status
            .success()
    );
    let repo = git2::Repository::open(&dest).unwrap();
    repo.set_head_detached(repo.head().unwrap().target().unwrap())
        .unwrap();
    assert_eq!(
        h.cmd()
            .current_dir(&dest)
            .args(["repository", "sync"])
            .output()
            .unwrap()
            .status
            .code(),
        Some(2)
    );
    let o = h
        .cmd()
        .current_dir(&dest)
        .args(["repository", "sync", "--ref", "main"])
        .output()
        .unwrap();
    assert!(o.status.success(), "{}", stderr(&o));
    assert!(repo.head_detached().unwrap());
}

#[test]
fn saved_ref_default_and_explicit_repository_namespace_are_honored() {
    let (h, http) = setup(Auth::None);
    configure(&h, &http.endpoint, "bearer");
    let dest = h.dir.path().join("catalog");
    assert!(
        h.run(&["repository", "clone", "catalog", dest.to_str().unwrap()])
            .status
            .success()
    );
    let repo = git2::Repository::open(&dest).unwrap();
    repo.config()
        .unwrap()
        .set_str("branch.main.merge", "refs/heads/other")
        .unwrap();
    let mut cfg = std::fs::read_to_string(h.config()).unwrap().replace(
        "namespace = \"shop\"",
        "namespace = \"shop\"\nref = \"main\"",
    );
    std::fs::write(h.config(), &cfg).unwrap();
    let o = h
        .cmd()
        .current_dir(&dest)
        .args(["repository", "sync"])
        .output()
        .unwrap();
    assert!(o.status.success(), "{}", stderr(&o));
    cfg = cfg.replace("namespace = \"shop\"", "namespace = \"different\"");
    std::fs::write(h.config(), cfg).unwrap();
    let count = http.calls.lock().unwrap().len();
    let o = h
        .cmd()
        .current_dir(&dest)
        .args(["repository", "sync", "catalog", "--remote", "gitstore"])
        .output()
        .unwrap();
    assert_eq!(o.status.code(), Some(2));
    assert_eq!(http.calls.lock().unwrap().len(), count);
}

#[test]
fn clone_flushes_trailing_progress_at_callback_completion() {
    let (h, http) = setup(Auth::Basic);
    *http.mode.lock().unwrap() = Mode::TrailingProgress;
    configure(&h, &http.endpoint, "basic");
    let destination = h.dir.path().join("clone");
    let output = h.run(&[
        "repository",
        "clone",
        "catalog",
        destination.to_str().unwrap(),
        "-o",
        "json",
    ]);
    assert!(output.status.success(), "{}", stderr(&output));
    assert!(
        stderr(&output).ends_with("hook pass"),
        "{}",
        stderr(&output)
    );
    assert!(!stderr(&output).contains("password"));
}
