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
    json!([e.context.endpoint, a.issuer, e.context.account, purpose]).to_string()
}
pub trait Store {
    fn read(&self, key: &str) -> Result<Option<Secret>>;
    fn remove(&self, key: &str) -> Result<bool>;
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
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn secret_is_single_line() {
        assert!(Secret::new("a\nb".into()).is_err());
        assert_eq!(Secret::new("token\r\n".into()).unwrap().expose(), "token");
    }
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
        if prompt.to_string() != "/" {
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
