mod support;
use clap::Parser;
use gs::{
    cli::Cli,
    config::{Account, Config, Context, Effective},
    git::{repository_url, target},
};
use std::collections::BTreeMap;
use support::*;
fn effective() -> Effective {
    let cli = Cli::parse_from(["gs", "context", "show"]);
    let context = Context {
        endpoint: "https://example.test/graphql".into(),
        account: "a".into(),
        contract_profile: Some("m0-2026-10-27".into()),
        namespace: Some("shop".into()),
        repository: None,
        git_ref: None,
        git_endpoint: Some("https://git.test/prefix".into()),
        git_auth: None,
        allow_loopback_http: false,
        accounts: BTreeMap::from([(
            "a".into(),
            Account {
                issuer: "test".into(),
                credential_source: "os-store".into(),
            },
        )]),
    };
    Config {
        version: 1,
        current_context: Some("test".into()),
        contexts: BTreeMap::from([("test".into(), context)]),
    }
    .effective(&cli)
    .unwrap()
}
#[test]
fn target_scope_uses_url_not_name() {
    let e = effective();
    assert_eq!(
        repository_url(&e, "catalog").unwrap(),
        "https://git.test/prefix/shop/catalog.git"
    );
    let t = target(&e, "https://git.test/prefix/shop/catalog.git").unwrap();
    assert_eq!(t.repository, "catalog");
    for url in [
        "https://git.test/other/shop/catalog.git",
        "https://other.test/prefix/shop/catalog.git",
        "file:///tmp/repo",
        "https://user:secret@git.test/prefix/shop/catalog.git",
        "https://git.test/prefix/shop%2Fother/catalog.git",
    ] {
        assert!(target(&e, url).is_err(), "{url}");
    }
}
#[test]
fn destination_preflight_without_git_or_credentials() {
    let h = Harness::new();
    h.configure("https://example.test/graphql");
    let dir = h.dir.path().join("existing");
    std::fs::create_dir(&dir).unwrap();
    std::fs::write(dir.join("keep"), "unchanged").unwrap();
    let o = h.run(&["repository", "clone", "catalog", dir.to_str().unwrap()]);
    assert_eq!(o.status.code(), Some(2));
    assert_eq!(
        std::fs::read_to_string(dir.join("keep")).unwrap(),
        "unchanged"
    );
}
