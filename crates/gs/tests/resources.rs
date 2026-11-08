mod support;
use gs::{
    cli::Selection,
    resources::{Kind, list_request, selection},
};
#[test]
fn exact_selectors_and_plural_fields() {
    for (k, plural) in [
        (Kind::Namespace, "namespaces"),
        (Kind::Repository, "repositories"),
        (Kind::Product, "products"),
        (Kind::Category, "categories"),
        (Kind::Collection, "collections"),
    ] {
        let r = list_request(k, Some("shop"), 100, None).unwrap();
        assert!(r.envelope["query"].as_str().unwrap().contains(plural));
    }
    let s = Selection {
        kind: "repository".into(),
        name: Some("catalog".into()),
        id: None,
    };
    let r = selection(&s, Some("shop")).unwrap();
    assert_eq!(
        r.envelope["variables"]["by"]["namespacePath"]["name"],
        "catalog"
    );
    let s = Selection {
        kind: "file".into(),
        name: Some("photo".into()),
        id: None,
    };
    let r = selection(&s, Some("shop")).unwrap();
    assert!(
        r.envelope["query"]
            .as_str()
            .unwrap()
            .contains("file(namespace:")
    );
    assert!(r.envelope["variables"].get("by").is_none());
    assert!(list_request(Kind::File, Some("shop"), 100, None).is_err());
}
#[test]
fn invalid_selectors_fail() {
    let s = Selection {
        kind: "product".into(),
        name: None,
        id: None,
    };
    assert!(selection(&s, Some("shop")).is_err());
    let s = Selection {
        kind: "product".into(),
        name: Some("p".into()),
        id: None,
    };
    assert!(selection(&s, None).is_err());
    assert!(list_request(Kind::Product, Some("shop"), 101, None).is_err());
}

#[test]
fn alpha9_file_selector_is_explicit_and_preserves_legacy_gates() {
    use gs::resources::selection_for_profile;
    let mut s = Selection {
        kind: "file".into(),
        name: Some("photo".into()),
        id: None,
    };
    let r = selection_for_profile(&s, Some("shop"), Some("gitstore-alpha9-2026-11-05")).unwrap();
    assert!(
        r.envelope["query"]
            .as_str()
            .unwrap()
            .contains("file(by:$by)")
    );
    assert_eq!(
        r.envelope["variables"],
        serde_json::json!({"by":{"namespacePath":{"namespace":"shop","name":"photo"}}})
    );
    let legacy = selection_for_profile(&s, Some("shop"), Some("m0-2026-10-27")).unwrap();
    assert!(
        legacy.envelope["query"]
            .as_str()
            .unwrap()
            .contains("file(namespace:")
    );
    assert!(selection_for_profile(&s, None, Some("gitstore-alpha9-2026-11-05")).is_err());
    s.name = None;
    s.id = Some("opaque-id".into());
    let by_id = selection_for_profile(&s, None, Some("gitstore-alpha9-2026-11-05")).unwrap();
    assert_eq!(
        by_id.envelope["variables"],
        serde_json::json!({"by":{"id":"opaque-id"}})
    );
}
