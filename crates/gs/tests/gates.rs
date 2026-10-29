mod support;
use support::*;
#[test]
fn unsupported_before_config_or_network() {
    let h = Harness::new();
    for args in [
        vec!["auth", "login"],
        vec!["auth", "refresh"],
        vec!["watch", "product"],
        vec!["api-resources"],
        vec!["explain", "product"],
        vec!["read", "product", "x", "-o", "yaml"],
        vec!["repository", "sync", "--wait", "ready"],
    ] {
        let o = h.run(&args);
        assert_eq!(o.status.code(), Some(6), "{args:?}: {}", stderr(&o));
    }
}
#[test]
fn usage_is_distinct() {
    let h = Harness::new();
    assert_eq!(h.run(&["unknown"]).status.code(), Some(2));
    assert_eq!(
        h.run(&["list", "product", "--first", "101"]).status.code(),
        Some(2)
    );
}
