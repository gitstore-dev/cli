mod support;
use serde_json::Value;
use support::{Harness, stderr};

#[test]
fn builtin_help_is_local_profiled_and_tracks_implemented_operations() {
    let h = Harness::new();
    h.write("config.toml", "invalid configuration must never be read");
    for profile in ["gitstore-alpha9-2026-11-05", "m0-2026-10-27"] {
        let before = std::fs::read_dir(h.dir.path()).unwrap().count();
        let out = h.run(&["api-resources", "--profile", profile, "-o", "json"]);
        assert!(out.status.success(), "{}", stderr(&out));
        let catalog: Value = serde_json::from_slice(&out.stdout).unwrap();
        assert_eq!(catalog["origin"], "local built-in");
        assert_eq!(catalog["profile"], profile);
        let entries = catalog["resources"].as_array().unwrap();
        assert_eq!(entries.len(), 6);
        let file = entries.iter().find(|item| item["name"] == "file").unwrap();
        assert_eq!(
            file["operations"]["readById"],
            profile.starts_with("gitstore-alpha9")
        );
        assert_eq!(file["operations"]["list"], false);
        assert!(entries.iter().all(|item| item["operations"]["watch"]
            == (profile.starts_with("gitstore-alpha9") && item["name"] != "collection")
            && item["operations"]["update"] == false
            && item["operations"]["watchById"] == false));
        assert_eq!(std::fs::read_dir(h.dir.path()).unwrap().count(), before);
    }
    let out = h.run(&["explain", "product.spec.title", "-o", "json"]);
    assert!(out.status.success(), "{}", stderr(&out));
    let value: Value = serde_json::from_slice(&out.stdout).unwrap();
    assert_eq!(value["definition"]["type"], "String!");
    assert_eq!(value["definition"]["writable"], true);
    let out = h.run(&["explain", "product.status.observedGeneration", "-o", "json"]);
    assert!(out.status.success(), "{}", stderr(&out));
    assert_eq!(
        serde_json::from_slice::<Value>(&out.stdout).unwrap()["definition"]["writable"],
        false
    );
    assert_eq!(
        h.run(&["explain", "product.spec.invented"]).status.code(),
        Some(2)
    );
    assert_eq!(
        h.run(&["explain", "custom-resource"]).status.code(),
        Some(6)
    );
}
