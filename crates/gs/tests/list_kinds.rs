//! `variant`, `serviceaccount` and `file` lists: which contract profile each needs, the
//! requests they send, and the operations that stay unsupported. Evidence: GitStore
//! v0.1.0-alpha.12 (66d9b3c13eacbed1b8487b8c4d6b0173b4cbcad2) shared/schemas/admin.
mod support;
use support::*;
use tiny_http::{Response, Server};

const ALPHA9: &str = "gitstore-alpha9-2026-11-05";
const ALPHA12: &str = "gitstore-alpha12-2026-11-26";
const M0: &str = "m0-2026-10-27";

fn configure(h: &Harness, endpoint: &str, profile: &str) -> String {
    h.write(
        "config.toml",
        &format!(
            r#"version = 1
current_context = "one"
[contexts.one]
endpoint = "{endpoint}"
allow_loopback_http = true
contract_profile = "{profile}"
account = "alice"
namespace = "shop"
[contexts.one.accounts.alice]
issuer = "test"
"#
        ),
    );
    h.write("token", "synthetic-only")
        .to_str()
        .unwrap()
        .to_owned()
}

/// Run `args` against a server that answers once; return the exit output and the request, if any.
fn run_with(
    profile: &str,
    reply: &'static str,
    args: &[&str],
) -> (std::process::Output, Option<serde_json::Value>) {
    let h = Harness::new();
    let server = Server::http("127.0.0.1:0").unwrap();
    let token = configure(
        &h,
        &format!("http://{}/graphql", server.server_addr()),
        profile,
    );
    let worker = std::thread::spawn(move || {
        let mut request = server
            .recv_timeout(std::time::Duration::from_millis(1500))
            .unwrap()?;
        let mut body = String::new();
        request.as_reader().read_to_string(&mut body).unwrap();
        request.respond(Response::from_string(reply)).unwrap();
        Some(serde_json::from_str(&body).unwrap())
    });
    let mut full = vec!["--credential-file", token.as_str()];
    full.extend_from_slice(args);
    let output = h.run(&full);
    (output, worker.join().unwrap())
}

const VARIANTS: &str = r#"{"data":{"productVariants":{"edges":[{"cursor":"c1","node":{"apiVersion":"catalog.gitstore.dev/v1beta1","kind":"ProductVariant","id":"v1","metadata":{"name":"hat-red","namespace":"shop"},"spec":{"title":"Red hat","sku":"HAT-R"},"status":{"conditions":[]}}}],"pageInfo":{"hasNextPage":false,"hasPreviousPage":false}}}}"#;
const VARIANT: &str = r#"{"data":{"productVariant":{"apiVersion":"catalog.gitstore.dev/v1beta1","kind":"ProductVariant","id":"v1","metadata":{"name":"hat-red","namespace":"shop"},"spec":{"title":"Red hat","sku":"HAT-R"},"status":{"conditions":[]}}}}"#;
const FILES: &str = r#"{"data":{"files":{"edges":[{"cursor":"c1","node":{"apiVersion":"gitstore.dev/v1beta1","kind":"File","id":"f1","metadata":{"name":"logo","namespace":"shop"},"spec":{"contentType":"image/png"},"status":{"conditions":[]}}}],"pageInfo":{"hasNextPage":false,"hasPreviousPage":false}}}}"#;
const ACCOUNTS: &str = r#"{"data":{"serviceAccounts":{"edges":[{"cursor":"c1","node":{"apiVersion":"gitstore.dev/v1beta1","kind":"ServiceAccount","id":"s1","metadata":{"name":"ci"},"keyIDs":["k1"],"groups":[],"lifecycle":{"state":"ACTIVE"}}}],"pageInfo":{"hasNextPage":false,"hasPreviousPage":false}}}}"#;

#[test]
fn variant_list_and_read_work_from_alpha9_and_use_the_namespaced_queries() {
    for profile in [ALPHA9, ALPHA12] {
        let (o, sent) = run_with(profile, VARIANTS, &["list", "variant", "-o", "table"]);
        assert!(o.status.success(), "{profile}: {}", stderr(&o));
        assert!(stdout(&o).contains("hat-red"), "{}", stdout(&o));
        let sent = sent.expect("a request");
        let query = sent["query"].as_str().unwrap();
        assert!(
            query.contains("productVariants(namespace:$namespace"),
            "{query}"
        );
        assert!(
            query.contains("sku") && query.contains("selectedOptions"),
            "{query}"
        );
        assert_eq!(sent["variables"]["namespace"], "shop");

        let (o, sent) = run_with(
            profile,
            VARIANT,
            &["read", "variant/hat-red", "-o", "table"],
        );
        assert!(o.status.success(), "{profile}: {}", stderr(&o));
        assert!(
            stdout(&o).contains("Kind:      ProductVariant"),
            "{}",
            stdout(&o)
        );
        let sent = sent.expect("a request");
        assert!(
            sent["query"]
                .as_str()
                .unwrap()
                .contains("productVariant(by:$by)")
        );
        assert_eq!(
            sent["variables"]["by"]["namespacePath"],
            serde_json::json!({"namespace": "shop", "name": "hat-red"})
        );
    }
}

#[test]
fn variant_needs_at_least_the_alpha9_profile() {
    for args in [vec!["list", "variant"], vec!["read", "variant", "hat-red"]] {
        let (o, sent) = run_with(M0, VARIANTS, &args);
        assert_eq!(o.status.code(), Some(6), "{args:?}: {}", stderr(&o));
        assert!(sent.is_none(), "{args:?} must fail before any request");
    }
}

#[test]
fn file_list_needs_the_alpha12_profile() {
    let (o, sent) = run_with(ALPHA12, FILES, &["list", "file", "-o", "table"]);
    assert!(o.status.success(), "{}", stderr(&o));
    assert!(stdout(&o).contains("logo"), "{}", stdout(&o));
    let sent = sent.expect("a request");
    assert!(
        sent["query"]
            .as_str()
            .unwrap()
            .contains("files(namespace:$namespace")
    );
    assert_eq!(sent["variables"]["namespace"], "shop");
    for profile in [M0, ALPHA9] {
        let (o, sent) = run_with(profile, FILES, &["list", "file"]);
        assert_eq!(o.status.code(), Some(6), "{profile}: {}", stderr(&o));
        assert!(stderr(&o).contains("DEP-007"), "{}", stderr(&o));
        assert!(sent.is_none(), "{profile} must fail before any request");
    }
}

#[test]
fn serviceaccount_list_is_cluster_scoped_and_needs_alpha12() {
    let (o, sent) = run_with(
        ALPHA12,
        ACCOUNTS,
        &["list", "serviceaccount", "-o", "table"],
    );
    assert!(o.status.success(), "{}", stderr(&o));
    assert!(stdout(&o).contains("ci"), "{}", stdout(&o));
    let sent = sent.expect("a request");
    let query = sent["query"].as_str().unwrap();
    assert!(
        query.contains("serviceAccounts(first:$first,after:$after)"),
        "{query}"
    );
    assert!(
        !query.contains("namespace"),
        "cluster-scoped list must not send a namespace: {query}"
    );
    assert!(sent["variables"].get("namespace").is_none());
    assert!(
        query.contains("keyIDs") && query.contains("lifecycle"),
        "{query}"
    );
    for profile in [M0, ALPHA9] {
        let (o, sent) = run_with(profile, ACCOUNTS, &["list", "serviceaccount"]);
        assert_eq!(o.status.code(), Some(6), "{profile}: {}", stderr(&o));
        assert!(sent.is_none(), "{profile} must fail before any request");
    }
}

#[test]
fn serviceaccount_has_no_single_read_and_no_mutations_here() {
    for args in [
        vec!["read", "serviceaccount", "ci"],
        vec!["read", "serviceaccount/ci"],
        vec!["delete", "serviceaccount", "ci"],
        vec!["delete", "variant", "hat-red"],
        vec!["delete", "file", "logo"],
    ] {
        let (o, sent) = run_with(ALPHA12, ACCOUNTS, &args);
        assert_eq!(o.status.code(), Some(6), "{args:?}: {}", stderr(&o));
        assert!(sent.is_none(), "{args:?} must fail before any request");
    }
}

#[test]
fn alpha12_profile_keeps_alpha9_projections_for_existing_kinds() {
    let (o, sent) = run_with(
        ALPHA12,
        r#"{"data":{"product":null}}"#,
        &["read", "product", "hat", "-o", "table"],
    );
    assert_eq!(o.status.code(), Some(4), "{}", stderr(&o));
    let query = sent.expect("a request")["query"]
        .as_str()
        .unwrap()
        .to_owned();
    assert!(
        query.contains("variantSummary"),
        "alpha.9 projection expected: {query}"
    );
}

#[test]
fn unknown_kinds_still_fail_unsupported() {
    let (o, sent) = run_with(ALPHA12, VARIANTS, &["list", "widget"]);
    assert_eq!(o.status.code(), Some(6), "{}", stderr(&o));
    assert!(sent.is_none());
}
