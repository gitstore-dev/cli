use serde::Serialize;
#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Error {
    pub code: &'static str,
    pub stage: &'static str,
    pub retryable: bool,
    pub message: String,
    #[serde(skip)]
    pub exit: i32,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub revision: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub submission: Option<String>,
}
pub type Result<T> = std::result::Result<T, Error>;
impl Error {
    pub fn new(exit: i32, code: &'static str, message: &str) -> Self {
        Self {
            exit,
            code,
            message: message.into(),
            stage: "preflight",
            retryable: false,
            revision: None,
            submission: None,
        }
    }
    pub fn usage(message: &str) -> Self {
        Self::new(2, "INVALID_INPUT", message)
    }
    pub fn auth(message: &str) -> Self {
        Self::new(3, "CREDENTIAL", message)
    }
    pub fn unsupported(dep: &str) -> Self {
        Self::new(
            6,
            "UNSUPPORTED",
            &format!("Unsupported capability; prerequisite {dep}"),
        )
    }
    pub fn remote(message: &str) -> Self {
        let mut e = Self::new(8, "REMOTE_FAILURE", message);
        e.stage = "transport";
        e
    }
    pub fn deadline() -> Self {
        Self::new(7, "DEADLINE_EXCEEDED", "Command deadline exceeded")
    }
    pub fn uncertain(mut self) -> Self {
        self.submission = Some("unknown".into());
        self
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn uncertainty_is_explicit() {
        let e = Error::remote("Response lost").uncertain();
        let v = serde_json::to_value(e).unwrap();
        assert_eq!(v["submission"], "unknown");
        assert!(v.get("revision").is_none());
        assert!(v.get("exit").is_none());
    }
}
