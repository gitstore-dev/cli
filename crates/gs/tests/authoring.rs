use gs::authoring::parse;
fn doc(kind: &str, extra: &str, body: &str) -> String {
    format!(
        "---\napiVersion: catalog.gitstore.dev/v1beta1\nkind: {kind}\nmetadata:\n  name: thing\n  namespace: shop\nspec:\n  title: Café\n{extra}---\n{body}"
    )
}
#[test]
fn exact_unicode_and_trailing_newlines() {
    for kind in ["Product", "CategoryTaxonomy"] {
        for body in ["", "é", "é\n", "é\n\n", "one\n---\ntwo\n"] {
            let a = parse(&doc(kind, "", body), Some("shop")).unwrap();
            assert_eq!(a.input["body"], body);
        }
    }
}
#[test]
fn rejects_output_fields_and_unknown_inputs() {
    for extra in ["status: {}\n", "spec2: {}\n", "body: illegal\n"] {
        assert!(parse(&doc("Product", extra, ""), Some("shop")).is_err());
    }
    assert!(parse(&doc("Product", "", ""), Some("other")).is_err());
    assert!(parse(&doc("Namespace", "", ""), Some("shop")).is_err());
}
#[test]
fn nested_types_and_duplicate_keys() {
    let text = doc("Product", "", "").replace(
        "title: Café",
        "title: Café\n  lifecycle:\n    state: INVALID",
    );
    assert!(parse(&text, Some("shop")).is_err());
    let text = doc("Product", "", "").replace("title: Café", "title: Café\n  title: again");
    assert!(parse(&text, Some("shop")).is_err());
}
#[test]
fn batch_count_limit_and_context_namespace_default() {
    let temp = tempfile::tempdir().unwrap();
    let p = temp.path().join("p.md");
    std::fs::write(&p, doc("Product", "", "")).unwrap();
    let files = vec![p.to_str().unwrap().to_string(); 100];
    assert!(gs::authoring::load(&files, Some("different-default"), false).is_ok());
    assert!(gs::authoring::load(&files, Some("different-explicit"), true).is_err());
    let files = vec![p.to_str().unwrap().to_string(); 101];
    assert!(gs::authoring::load(&files, Some("shop"), true).is_err());
}
