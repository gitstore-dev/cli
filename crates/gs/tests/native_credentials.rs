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

#[test]
#[ignore = "Requires explicit opt-in to the native nonprompt session store; unique synthetic identity only"]
fn native_session_create_replace_read_and_remove() {
    use gs::credentials::{Native, Secret, Store};
    struct Cleanup(String);
    impl Drop for Cleanup {
        fn drop(&mut self) {
            let _ = Native.remove(&self.0);
        }
    }
    let key = format!(
        "gitstore-synthetic-session-fixture-{}-{}",
        std::process::id(),
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_nanos()
    );
    let _cleanup = Cleanup(key.clone());
    assert!(Native.read(&key).unwrap().is_none());
    for value in ["synthetic-session-only-one", "synthetic-session-only-two"] {
        Native
            .write_session(&key, &Secret::new(value.into()).unwrap())
            .unwrap();
        let read = Native.read(&key).unwrap().unwrap();
        assert!(read.expose() == value, "Native synthetic session differs");
    }
    assert!(Native.remove(&key).unwrap());
    assert!(Native.read(&key).unwrap().is_none());
}
