mod support;
use gs::{cli::Selection, resources::selection_for_profile};
use serde_json::json;

const ALPHA9: Option<&str> = Some("gitstore-alpha9-2026-11-05");

#[test]
fn scoped_creation_uses_exact_inputs_and_namespace_boundaries() {
    use gs::authoring::parse_for_profile;
    let namespace = "---\napiVersion: gitstore.dev/v1beta1\nkind: Namespace\nmetadata:\n  name: acme\nspec:\n  tier: ORGANIZATION\n  repositoryDefaults:\n    defaultBranch: main\n---\n";
    let document = parse_for_profile(namespace, Some("ignored-context"), false, ALPHA9).unwrap();
    assert!(document.input.get("body").is_none());
    assert!(document.input["metadata"].get("namespace").is_none());
    assert_eq!(document.input["spec"]["tier"], "ORGANIZATION");
    assert!(parse_for_profile(namespace, Some("explicit"), true, ALPHA9).is_err());
    assert!(parse_for_profile(namespace, None, false, None).is_err());
    for body in ["body", "\n", " "] {
        assert!(parse_for_profile(&format!("{namespace}{body}"), None, false, ALPHA9).is_err());
    }
    for replacement in [
        "tier: INVALID",
        "tier: USER\n  unknown: true",
        "tier: USER\n  pushPolicyDefaults:\n    receivePackHooks: {}",
    ] {
        assert!(
            parse_for_profile(
                &namespace.replace("tier: ORGANIZATION", replacement),
                None,
                false,
                ALPHA9
            )
            .is_err()
        );
    }
    let repository = "---\napiVersion: gitstore.dev/v1beta1\nkind: Repository\nmetadata:\n  name: catalog\nspec:\n  defaultBranch: main\n  visibility: PRIVATE\n  storageClass: standard\n---\n";
    let doc = parse_for_profile(repository, Some("shop"), true, ALPHA9).unwrap();
    assert_eq!(doc.input["metadata"]["namespace"], "shop");
    assert!(doc.input.get("body").is_none());
    assert!(parse_for_profile(repository, None, false, ALPHA9).is_err());
    for unknown in ["status: {}\n", "unknown: true\n"] {
        assert!(
            parse_for_profile(
                &repository.replacen("spec:\n", &format!("{unknown}spec:\n"), 1),
                Some("shop"),
                true,
                ALPHA9
            )
            .is_err()
        );
    }
}

#[test]
fn hydrated_core_projections_include_observed_state_only_in_alpha9() {
    for kind in [
        "namespace",
        "repository",
        "product",
        "category",
        "collection",
        "file",
    ] {
        let selection = Selection {
            kind: kind.into(),
            name: Some("fixture".into()),
            id: None,
        };
        let request = selection_for_profile(&selection, Some("shop"), ALPHA9).unwrap();
        let query = request.envelope["query"].as_str().unwrap();
        for required in [
            "labels",
            "annotations",
            "ownerReferences",
            "creationTimestamp",
            "deletionTimestamp",
            "spec {",
            "status {",
            "conditions {",
            "body",
        ] {
            assert!(query.contains(required), "{kind} missing {required}");
        }
        let legacy = selection_for_profile(&selection, Some("shop"), None).unwrap();
        assert!(
            !legacy.envelope["query"]
                .as_str()
                .unwrap()
                .contains("status {")
        );
    }
}

#[test]
fn file_id_requires_no_namespace_and_stays_opaque() {
    let selection = Selection {
        kind: "file".into(),
        name: None,
        id: Some("RmlsZToxL+opaque==".into()),
    };
    let request = selection_for_profile(&selection, None, ALPHA9).unwrap();
    assert_eq!(
        request.envelope["variables"],
        json!({"by":{"id":"RmlsZToxL+opaque=="}})
    );
    assert!(
        request.envelope["query"]
            .as_str()
            .unwrap()
            .contains("file(by:$by)")
    );
    assert!(selection_for_profile(&selection, None, None).is_err());
    assert!(selection_for_profile(&selection, None, Some("m0-2026-10-27")).is_err());
}

#[test]
fn file_selectors_fail_before_transport_when_ambiguous_empty_or_unscoped() {
    for (name, id) in [
        (None, None),
        (Some("name"), Some("id")),
        (Some(""), None),
        (None, Some("")),
        (Some("name"), None),
    ] {
        let selection = Selection {
            kind: "file".into(),
            name: name.map(str::to_owned),
            id: id.map(str::to_owned),
        };
        assert!(selection_for_profile(&selection, None, ALPHA9).is_err());
    }
}
