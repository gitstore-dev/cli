mod support;
use support::*;

#[test]
fn reference_generation_is_local_and_has_no_setup_side_effects() {
    let h = Harness::new();
    for args in [
        vec!["completion", "bash"],
        vec!["completion", "zsh"],
        vec!["completion", "fish"],
        vec!["reference"],
    ] {
        let result = h.run(&args);
        assert!(result.status.success(), "{}", stderr(&result));
        assert!(result.stderr.is_empty());
        assert!(stdout(&result).contains("gs"));
        assert!(!stdout(&result).contains('\u{1b}'));
    }
    assert_eq!(std::fs::read_dir(h.dir.path()).unwrap().count(), 0);
    let result = h.run(&["completion", "unsupported-shell"]);
    assert_eq!(result.status.code(), Some(2));
    let result = h.run(&["reference"]);
    let reference = stdout(&result);
    assert!(reference.contains("upstream GitStore schemas continue changing incompatibly"));
    assert!(
        reference.contains("No upgrade compatibility, data preservation or recovery guarantees")
    );
    assert!(reference.contains("Keep independent backups"));
}
