mod support;
use support::*;
#[test]
fn unsupported_before_config_or_network() {
    let h = Harness::new();
    for args in [
        vec!["watch", "collection"],
        vec!["watch", "product", "--id", "opaque"],
        vec!["update", "-f", "/must-not-open"],
        vec!["list", "file"],
        vec!["explain", "unregistered-crd"],
        vec!["repository", "sync", "--wait", "ready"],
    ] {
        let o = h.run(&args);
        assert_eq!(o.status.code(), Some(6), "{args:?}: {}", stderr(&o));
    }
}
#[test]
fn legacy_yaml_and_invalid_authoring_export_fail_before_credentials_or_transport() {
    let h = Harness::new();
    h.configure("http://127.0.0.1:1/graphql");
    assert_eq!(
        h.run(&["read", "product", "x", "-o", "yaml"]).status.code(),
        Some(6)
    );
    assert_eq!(
        h.run(&["read", "collection", "x", "--authoring", "-o", "markdown"])
            .status
            .code(),
        Some(6)
    );
    assert_eq!(
        h.run(&["read", "product", "x", "--authoring", "-o", "json"])
            .status
            .code(),
        Some(2)
    );
}
#[test]
fn usage_is_distinct() {
    let h = Harness::new();
    assert_eq!(h.run(&["unknown"]).status.code(), Some(2));
    assert_eq!(h.run(&["auth", "login"]).status.code(), Some(2));
    assert_eq!(
        h.run(&["list", "product", "--first", "101"]).status.code(),
        Some(2)
    );
}
