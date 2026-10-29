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
