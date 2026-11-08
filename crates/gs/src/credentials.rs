use crate::{
    cli::Cli,
    config::Effective,
    error::{Error, Result},
    input,
};
use serde_json::json;
use zeroize::Zeroizing;
pub struct Secret(Zeroizing<String>);
impl Secret {
    pub fn new(s: String) -> Result<Self> {
        let s = Zeroizing::new(s);
        let s = Zeroizing::new(
            s.strip_suffix("\r\n")
                .or_else(|| s.strip_suffix('\n'))
                .unwrap_or(&s)
                .to_owned(),
        );
        if s.is_empty() || s.chars().any(char::is_control) {
            return Err(Error::auth(
                "Credential must be nonempty single-line material",
            ));
        }
        Ok(Self(s))
    }
    pub fn expose(&self) -> &str {
        &self.0
    }
}
pub fn key(e: &Effective, purpose: &str) -> String {
    let a = &e.context.accounts[&e.context.account];
    if purpose == "graphql"
        && let Some(auth) = &a.auth
    {
        return json!([e.context.endpoint, a.issuer, e.context.account, purpose,
            {"provider":auth.provider,"version":auth.version,"username":auth.username}])
        .to_string();
    }
    json!([e.context.endpoint, a.issuer, e.context.account, purpose]).to_string()
}
pub trait Store {
    fn read(&self, key: &str) -> Result<Option<Secret>>;
    fn remove(&self, key: &str) -> Result<bool>;
    fn write_session(&self, _: &str, _: &Secret) -> Result<()> {
        Err(Error::unsupported(
            "session persistence in this credential store",
        ))
    }
    /// Private runtime bootstrap only. Implementations must never replace an
    /// existing credential, unlock a collection or open an authorization UI.
    fn insert_runtime_if_absent(&self, _: &str, _: &Secret) -> Result<()> {
        Err(Error::unsupported(
            "runtime credential creation in this store",
        ))
    }
}
pub struct Native;
#[cfg(target_os = "macos")]
fn entry(key: &str) -> Result<keyring_core::Entry> {
    #[cfg(target_os = "macos")]
    let store = {
        // CLI credential access must fail instead of opening a Keychain UI prompt.
        let status =
            unsafe { security_framework_sys::keychain::SecKeychainSetUserInteractionAllowed(0) };
        if status != 0 {
            return Err(Error::auth("Cannot disable credential-store interaction"));
        }
        apple_native_keyring_store::keychain::Store::new()
    };
    let store=store.map_err(|_|Error::auth("OS credential store unavailable; use an explicit protected file in headless environments"))?;
    keyring_core::set_default_store(store);
    keyring_core::Entry::new("dev.gitstore.gs", key)
        .map_err(|_| Error::auth("Cannot access OS credential entry"))
}
impl Store for Native {
    fn write_session(&self, key: &str, value: &Secret) -> Result<()> {
        #[cfg(target_os = "macos")]
        {
            entry(key)?
                .set_password(value.expose())
                .map_err(|_| Error::auth("OS session persistence denied or unavailable"))
        }
        #[cfg(target_os = "linux")]
        {
            use dbus_secret_service::{EncryptionType, SecretService};
            use std::collections::HashMap;
            let service = SecretService::connect_with_max_prompt_timeout(EncryptionType::Dh, 0)
                .map_err(|_| Error::auth("Linux Secret Service unavailable"))?;
            let attributes = HashMap::from([("service", "dev.gitstore.gs"), ("user", key)]);
            let matches = service
                .search_items(attributes.clone())
                .map_err(|_| Error::auth("Cannot inspect session credential"))?;
            if !matches.locked.is_empty() || matches.unlocked.len() > 1 {
                return Err(Error::auth(
                    "Session credential is locked or ambiguous; resolve outside gs",
                ));
            }
            if let Some(item) = matches.unlocked.first() {
                return item
                    .set_secret(value.expose().as_bytes(), "text/plain")
                    .map_err(|_| Error::auth("Cannot atomically update session credential"));
            }
            let collection = service
                .get_default_collection()
                .map_err(|_| Error::auth("No default session credential collection"))?;
            if collection
                .is_locked()
                .map_err(|_| Error::auth("Cannot inspect credential collection"))?
            {
                return Err(Error::auth(
                    "Credential collection is locked; unlock outside gs",
                ));
            }
            collection
                .create_item(
                    "GitStore session",
                    attributes,
                    value.expose().as_bytes(),
                    true,
                    "text/plain",
                )
                .map_err(|_| Error::auth("Cannot persist issued session"))?;
            Ok(())
        }
        #[cfg(not(any(target_os = "macos", target_os = "linux")))]
        {
            let _ = (key, value);
            Err(Error::unsupported("native session credential store"))
        }
    }
    fn insert_runtime_if_absent(&self, key: &str, value: &Secret) -> Result<()> {
        if self.read(key)?.is_some() {
            return Ok(());
        }
        #[cfg(target_os = "linux")]
        {
            use dbus_secret_service::{EncryptionType, SecretService};
            use std::collections::HashMap;
            // In this pinned library timeout=0 returns before Prompt.Prompt.
            // Do not call ensure_unlocked/unlock or create a collection.
            let service = SecretService::connect_with_max_prompt_timeout(EncryptionType::Dh, 0)
                .map_err(|_| Error::auth("Linux Secret Service unavailable"))?;
            let collection = service
                .get_default_collection()
                .map_err(|_| Error::auth("No default credential collection"))?;
            if collection
                .is_locked()
                .map_err(|_| Error::auth("Cannot inspect credential collection"))?
            {
                return Err(Error::auth(
                    "Credential collection is locked; unlock it outside gs",
                ));
            }
            collection
                .create_item(
                    "GitStore runtime protected material",
                    HashMap::from([("service", "dev.gitstore.gs"), ("user", key)]),
                    value.expose().as_bytes(),
                    false,
                    "text/plain",
                )
                .map_err(|_| {
                    Error::auth(
                        "Runtime credential creation denied or requires external authorization",
                    )
                })?;
            Ok(())
        }
        #[cfg(target_os = "macos")]
        {
            // Add-only Keychain API: unlike set_password it cannot overwrite a
            // credential inserted between the read and this call.
            let interaction = unsafe {
                security_framework_sys::keychain::SecKeychainSetUserInteractionAllowed(0)
            };
            if interaction != 0 {
                return Err(Error::auth("Cannot disable credential-store interaction"));
            }
            let service = b"dev.gitstore.gs";
            let status = unsafe {
                security_framework_sys::keychain::SecKeychainAddGenericPassword(
                    std::ptr::null_mut(),
                    service.len() as u32,
                    service.as_ptr().cast(),
                    key.len() as u32,
                    key.as_ptr().cast(),
                    value.expose().len() as u32,
                    value.expose().as_ptr().cast(),
                    std::ptr::null_mut(),
                )
            };
            if status != 0 && status != -25299 {
                // errSecDuplicateItem
                return Err(Error::auth(
                    "Runtime credential creation denied or unavailable",
                ));
            }
            Ok(())
        }
        #[cfg(not(any(target_os = "linux", target_os = "macos")))]
        {
            let _ = (key, value);
            Err(Error::unsupported("native runtime credential store"))
        }
    }
    fn read(&self, key: &str) -> Result<Option<Secret>> {
        #[cfg(target_os = "linux")]
        {
            linux(key, false)
        }
        #[cfg(target_os = "macos")]
        {
            match entry(key)?.get_password() {
                Ok(s) => Secret::new(s).map(Some),
                Err(keyring_core::error::Error::NoEntry) => Ok(None),
                Err(_) => Err(Error::auth(
                    "OS credential store unavailable or access denied",
                )),
            }
        }
        #[cfg(not(any(target_os = "macos", target_os = "linux")))]
        {
            let _ = key;
            Err(Error::unsupported(
                "native credential backend for this host",
            ))
        }
    }
    fn remove(&self, key: &str) -> Result<bool> {
        #[cfg(target_os = "linux")]
        {
            linux(key, true).map(|value| value.is_some())
        }
        #[cfg(target_os = "macos")]
        {
            match entry(key)?.delete_credential() {
                Ok(()) => Ok(true),
                Err(keyring_core::error::Error::NoEntry) => Ok(false),
                Err(_) => Err(Error::auth("Cannot remove OS credential entry")),
            }
        }
        #[cfg(not(any(target_os = "macos", target_os = "linux")))]
        {
            let _ = key;
            Err(Error::unsupported(
                "native credential backend for this host",
            ))
        }
    }
}
pub fn token(cli: &Cli, e: &Effective) -> Result<Secret> {
    if let Some(p) = &cli.credential_file {
        return Secret::new(input::protected(p)?);
    }
    if cli.credential_stdin {
        return Secret::new(input::read("-")?);
    }
    Native.read(&key(e,"graphql"))?.ok_or_else(||Error::auth("No credential for selected installation/issuer/account; supply protected input or provision the OS store"))
}

#[cfg(target_os = "linux")]
fn linux(key: &str, remove: bool) -> Result<Option<Secret>> {
    use dbus_secret_service::{EncryptionType, SecretService};
    use std::collections::HashMap;
    let service = SecretService::connect_with_max_prompt_timeout(EncryptionType::Dh, 0)
        .map_err(|_| Error::auth("Linux Secret Service unavailable"))?;
    let matches = service
        .search_items(HashMap::from([
            ("service", "dev.gitstore.gs"),
            ("user", key),
        ]))
        .map_err(|_| Error::auth("Cannot search Linux Secret Service"))?;
    // The generic keyring adapter automatically unlocks locked matches. Search
    // directly so non-interactive execution never calls Unlock or Prompt.
    if !matches.locked.is_empty() {
        return Err(Error::auth("Credential is locked; unlock it outside gs"));
    }
    if matches.unlocked.len() > 1 {
        return Err(Error::auth("Ambiguous OS credential entries"));
    }
    let Some(item) = matches.unlocked.first() else {
        return Ok(None);
    };
    if remove {
        let connection = dbus::blocking::Connection::new_session()
            .map_err(|_| Error::auth("Session bus unavailable"))?;
        let proxy = connection.with_proxy(
            "org.freedesktop.secrets",
            item.path.clone(),
            std::time::Duration::from_secs(5),
        );
        let (prompt,): (dbus::Path<'static>,) = proxy
            .method_call("org.freedesktop.Secret.Item", "Delete", ())
            .map_err(|_| Error::auth("Credential removal denied"))?;
        if prompt != "/" {
            // A returned prompt is not displayed unless Prompt.Prompt is invoked.
            // Dismiss it and report the required external action instead.
            let p = connection.with_proxy(
                "org.freedesktop.secrets",
                prompt,
                std::time::Duration::from_secs(5),
            );
            let _: std::result::Result<(), dbus::Error> =
                p.method_call("org.freedesktop.Secret.Prompt", "Dismiss", ());
            return Err(Error::auth(
                "Credential removal requires external authorization",
            ));
        }
        return Secret::new("removed".into()).map(Some);
    }
    let bytes = Zeroizing::new(
        item.get_secret()
            .map_err(|_| Error::auth("Credential access denied or locked"))?,
    );
    let text = std::str::from_utf8(&bytes).map_err(|_| Error::auth("Credential is not UTF-8"))?;
    Secret::new(text.to_owned()).map(Some)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn secret_is_single_line() {
        assert!(Secret::new("a\nb".into()).is_err());
        assert_eq!(Secret::new("token\r\n".into()).unwrap().expose(), "token");
    }
}
