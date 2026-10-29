#[test]
#[ignore = "Requires explicit opt-in to a real OS store; uses only a disposable synthetic entry"]
fn native_round_trip() {
    #[cfg(target_os = "macos")]
    let store = apple_native_keyring_store::keychain::Store::new().unwrap();
    #[cfg(target_os = "linux")]
    let store = dbus_secret_service_keyring_store::Store::new().unwrap();
    keyring_core::set_default_store(store);
    let entry = keyring_core::Entry::new(
        "dev.gitstore.gs.tests",
        &format!("smoke-{}", std::process::id()),
    )
    .unwrap();
    entry.set_password("synthetic-smoke-only").unwrap();
    let read = entry.get_password();
    let deleted = entry.delete_credential();
    assert_eq!(read.unwrap(), "synthetic-smoke-only");
    deleted.unwrap();
}
