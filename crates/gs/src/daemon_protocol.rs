//! Bounded local protocol JSON and framing. No runtime or remote API dependency.
use serde::{
    Deserialize,
    de::{self, DeserializeSeed, MapAccess, SeqAccess, Visitor},
};
use serde_json::{Map, Value};
use std::{
    fmt,
    io::{self, Read, Write},
};

pub const FRAME_MAX: usize = 1_048_576;
pub const STRING_MAX: usize = 262_144;
pub const ITEMS_MAX: usize = 4096;
pub const DEPTH_MAX: usize = 32;
pub const SAFE_INTEGER: i64 = 9_007_199_254_740_991;
const PARSE_BUDGET: usize = 16 * 1024 * 1024;

#[derive(Debug)]
pub struct WireError;
impl fmt::Display for WireError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str("Invalid or over-limit local protocol message")
    }
}
impl std::error::Error for WireError {}

#[derive(Debug, Clone, Deserialize)]
#[serde(try_from = "String")]
pub struct Identifier(pub String);
impl TryFrom<String> for Identifier {
    type Error = WireError;
    fn try_from(s: String) -> Result<Self, Self::Error> {
        if s.is_empty()
            || s.len() > 64
            || !s
                .bytes()
                .all(|c| c.is_ascii_alphanumeric() || b"._:-".contains(&c))
        {
            Err(WireError)
        } else {
            Ok(Self(s))
        }
    }
}
#[derive(Debug, Clone, Copy, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Version {
    pub major: u32,
    pub minor: u32,
}
#[derive(Debug, Clone, Copy, Deserialize)]
pub enum Method {
    #[serde(rename = "system.negotiate")]
    Negotiate,
    #[serde(rename = "system.capabilities")]
    Capabilities,
    #[serde(rename = "deployment.validate")]
    Validate,
    #[serde(rename = "deployment.start")]
    Start,
    #[serde(rename = "deployment.inspect")]
    Inspect,
    #[serde(rename = "deployment.list")]
    List,
    #[serde(rename = "deployment.stop")]
    Stop,
    #[serde(rename = "operation.inspect")]
    OperationInspect,
    #[serde(rename = "operation.wait")]
    Wait,
    #[serde(rename = "operation.cancel")]
    Cancel,
    #[serde(rename = "migration.start")]
    Migrate,
    #[serde(rename = "stream.subscribe")]
    Subscribe,
    #[serde(rename = "stream.unsubscribe")]
    Unsubscribe,
}
#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Request {
    pub version: Version,
    pub request_id: Identifier,
    pub method: Method,
    pub params: Map<String, Value>,
    pub idempotency_key: Option<Identifier>,
    pub timeout_ms: Option<u32>,
}
#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct NegotiateParams {
    pub supported: Vec<Version>,
    pub required_capabilities: Vec<String>,
}
#[derive(Debug, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum OperationState {
    Accepted,
    Running,
    Succeeded,
    Failed,
    Cancelled,
}
#[derive(Debug, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Recovery {
    Normal,
    Reconciling,
    Uncertain,
}

// Structural/numeric preflight does not parse JSON: serde still validates its grammar.
// It bounds recursive materialization and prevents arbitrary_precision's number
// representation from turning forbidden reals into synthetic map values.
fn preflight(bytes: &[u8]) -> Result<(), WireError> {
    if bytes.is_empty() || bytes.len() > FRAME_MAX {
        return Err(WireError);
    }
    let (mut depth, mut string, mut i) = (0usize, false, 0usize);
    while i < bytes.len() {
        let c = bytes[i];
        if string {
            if c == b'\\' {
                i += 1;
                if i == bytes.len() {
                    return Err(WireError);
                }
            } else if c == b'"' {
                string = false;
            }
        } else {
            match c {
                b'"' => string = true,
                b'{' | b'[' => {
                    depth += 1;
                    if depth > DEPTH_MAX {
                        return Err(WireError);
                    }
                }
                b'}' | b']' => {
                    depth = depth.checked_sub(1).ok_or(WireError)?;
                }
                b'-' | b'0'..=b'9' => {
                    let start = i;
                    while i < bytes.len()
                        && (bytes[i].is_ascii_digit() || b"-+.eE".contains(&bytes[i]))
                    {
                        i += 1;
                    }
                    let n = std::str::from_utf8(&bytes[start..i]).map_err(|_| WireError)?;
                    if n.bytes().any(|v| b"+.eE".contains(&v)) {
                        return Err(WireError);
                    }
                    let n = n.parse::<i64>().map_err(|_| WireError)?;
                    if !(-SAFE_INTEGER..=SAFE_INTEGER).contains(&n) {
                        return Err(WireError);
                    }
                    continue;
                }
                _ => {}
            }
        }
        i += 1;
    }
    if depth == 0 && !string {
        Ok(())
    } else {
        Err(WireError)
    }
}
struct Seed<'a>(&'a mut usize);
fn spend<E: de::Error>(remaining: &mut usize, amount: usize) -> Result<(), E> {
    *remaining = remaining
        .checked_sub(amount)
        .ok_or_else(|| E::custom("allocation budget"))?;
    Ok(())
}
impl<'de> DeserializeSeed<'de> for Seed<'_> {
    type Value = Value;
    fn deserialize<D: de::Deserializer<'de>>(self, d: D) -> Result<Value, D::Error> {
        spend(self.0, 64)?;
        d.deserialize_any(self)
    }
}
impl<'de> Visitor<'de> for Seed<'_> {
    type Value = Value;
    fn expecting(&self, f: &mut fmt::Formatter) -> fmt::Result {
        f.write_str("bounded JSON value")
    }
    fn visit_bool<E: de::Error>(self, v: bool) -> Result<Value, E> {
        Ok(v.into())
    }
    fn visit_unit<E: de::Error>(self) -> Result<Value, E> {
        Ok(Value::Null)
    }
    fn visit_i64<E: de::Error>(self, v: i64) -> Result<Value, E> {
        if !(-SAFE_INTEGER..=SAFE_INTEGER).contains(&v) {
            return Err(E::custom("integer bound"));
        }
        Ok(v.into())
    }
    fn visit_u64<E: de::Error>(self, v: u64) -> Result<Value, E> {
        if v > SAFE_INTEGER as u64 {
            return Err(E::custom("integer bound"));
        }
        Ok(v.into())
    }
    fn visit_str<E: de::Error>(self, v: &str) -> Result<Value, E> {
        if v.len() > STRING_MAX {
            return Err(E::custom("string bound"));
        }
        spend(self.0, v.len())?;
        Ok(Value::String(v.into()))
    }
    fn visit_seq<A: SeqAccess<'de>>(self, mut a: A) -> Result<Value, A::Error> {
        let mut v = Vec::new();
        while let Some(item) = a.next_element_seed(Seed(self.0))? {
            if v.len() >= ITEMS_MAX {
                return Err(de::Error::custom("item bound"));
            }
            v.push(item);
        }
        Ok(Value::Array(v))
    }
    fn visit_map<A: MapAccess<'de>>(self, mut a: A) -> Result<Value, A::Error> {
        let mut v = Map::new();
        while let Some(key) = a.next_key::<String>()? {
            if key.len() > STRING_MAX || v.len() >= ITEMS_MAX || v.contains_key(&key) {
                return Err(de::Error::custom("object key bound or duplicate"));
            }
            spend(self.0, key.len() + 64)?;
            v.insert(key, a.next_value_seed(Seed(self.0))?);
        }
        Ok(Value::Object(v))
    }
}
pub fn decode(bytes: &[u8]) -> Result<Value, WireError> {
    preflight(bytes)?;
    let mut deserializer = serde_json::Deserializer::from_slice(bytes);
    let mut budget = PARSE_BUDGET;
    let value = Seed(&mut budget)
        .deserialize(&mut deserializer)
        .map_err(|_| WireError)?;
    deserializer.end().map_err(|_| WireError)?;
    if !value.is_object() {
        return Err(WireError);
    }
    Ok(value)
}
pub fn read_frame<R: Read>(reader: &mut R) -> io::Result<Option<Value>> {
    let mut prefix = [0u8; 4];
    loop {
        match reader.read(&mut prefix[..1]) {
            Ok(0) => return Ok(None),
            Ok(_) => break,
            Err(e) if e.kind() == io::ErrorKind::Interrupted => continue,
            Err(e) => return Err(e),
        }
    }
    reader.read_exact(&mut prefix[1..])?;
    let length = u32::from_be_bytes(prefix) as usize;
    if length == 0 || length > FRAME_MAX {
        return Err(io::Error::new(io::ErrorKind::InvalidData, WireError));
    }
    let mut payload = vec![0; length];
    reader.read_exact(&mut payload)?;
    decode(&payload)
        .map(Some)
        .map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e))
}
pub fn write_frame<W: Write>(writer: &mut W, value: &Value) -> io::Result<()> {
    let payload = serde_json::to_vec(value)?;
    decode(&payload).map_err(|e| io::Error::new(io::ErrorKind::InvalidInput, e))?;
    writer.write_all(&(payload.len() as u32).to_be_bytes())?;
    writer.write_all(&payload)?;
    writer.flush()
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Component {
    Api,
    Git,
    Controller,
    Oidc,
    Admin,
    Storefront,
}
#[derive(Debug, Clone, Copy, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Datastore {
    Memdb,
    Scylladb,
}
#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct ProfileParams {
    pub profile_id: Identifier,
    pub profile_version: Identifier,
    pub components: Option<Vec<Component>>,
    pub datastore: Option<Datastore>,
    pub deployment_id: Option<Identifier>,
    pub allow_provision: Option<bool>,
}
#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct StartParams {
    pub deployment_id: Identifier,
    pub profile_id: Identifier,
    pub profile_version: Identifier,
    pub components: Option<Vec<Component>>,
    pub datastore: Option<Datastore>,
    pub allow_provision: Option<bool>,
    pub asset_manifest_digest: Option<String>,
}
#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct DeploymentParams {
    pub deployment_id: Identifier,
}
#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct StopParams {
    pub deployment_id: Identifier,
    pub expected_operation_id: Option<Identifier>,
}
#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct OperationParams {
    pub operation_id: Identifier,
}
#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct ListParams {
    pub limit: Option<u32>,
    pub cursor: Option<String>,
}
#[derive(Debug, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Until {
    Accepted,
    Terminal,
    Ready,
}
#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct WaitParams {
    pub operation_id: Identifier,
    pub until: Until,
}
#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct MigrationParams {
    pub deployment_id: Identifier,
    pub target_version: Identifier,
    pub backup_acknowledged: bool,
}
#[derive(Debug, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum StreamKind {
    Logs,
    Events,
}
#[derive(Debug, Deserialize)]
#[serde(untagged)]
pub enum LogComponent {
    Service(Component),
    Provider(ProviderLogComponent),
}
#[derive(Debug, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum ProviderLogComponent {
    OidcSmtp,
    OidcHydra,
    OidcKratos,
    OidcUi,
    OidcBridge,
    Scylladb,
}
#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct SubscribeParams {
    pub deployment_id: Identifier,
    pub kind: StreamKind,
    pub component: Option<LogComponent>,
    pub cursor: Option<String>,
}
#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct UnsubscribeParams {
    pub subscription_id: Identifier,
}
#[derive(Debug)]
pub enum Params {
    Negotiate(NegotiateParams),
    Empty,
    Validate(ProfileParams),
    Start(StartParams),
    Deployment(DeploymentParams),
    Stop(StopParams),
    List(ListParams),
    Operation(OperationParams),
    Wait(WaitParams),
    Migrate(MigrationParams),
    Subscribe(SubscribeParams),
    Unsubscribe(UnsubscribeParams),
}
fn typed<T: serde::de::DeserializeOwned>(value: &Value) -> Result<T, WireError> {
    serde_json::from_value(value.clone()).map_err(|_| WireError)
}
fn components_valid(items: Option<&[Component]>) -> bool {
    items.is_none_or(|a| a.len() <= 6 && a.iter().enumerate().all(|(i, v)| !a[..i].contains(v)))
}
pub fn request(value: &Value) -> Result<(Request, Params), WireError> {
    for key in ["idempotencyKey", "timeoutMs"] {
        if value.get(key).is_some_and(Value::is_null) {
            return Err(WireError);
        }
    }
    let request: Request = typed(value)?;
    if request.timeout_ms.is_some_and(|n| n == 0 || n > 3_600_000)
        || request.params.values().any(Value::is_null)
    {
        return Err(WireError);
    }
    if matches!(
        request.method,
        Method::Start | Method::Stop | Method::Cancel | Method::Migrate
    ) && request.idempotency_key.is_none()
    {
        return Err(WireError);
    }
    let value = Value::Object(request.params.clone());
    let params = match request.method {
        Method::Negotiate => {
            let p: NegotiateParams = typed(&value)?;
            if p.supported.is_empty()
                || p.supported.len() > 16
                || p.required_capabilities.len() > 64
                || p.required_capabilities
                    .iter()
                    .enumerate()
                    .any(|(i, v)| v.is_empty() || p.required_capabilities[..i].contains(v))
            {
                return Err(WireError);
            }
            Params::Negotiate(p)
        }
        Method::Capabilities => {
            if !request.params.is_empty() {
                return Err(WireError);
            }
            Params::Empty
        }
        Method::Validate => {
            let p: ProfileParams = typed(&value)?;
            if !components_valid(p.components.as_deref()) {
                return Err(WireError);
            }
            Params::Validate(p)
        }
        Method::Start => {
            let p: StartParams = typed(&value)?;
            if !components_valid(p.components.as_deref())
                || p.asset_manifest_digest.as_ref().is_some_and(|digest| {
                    digest.len() != 71
                        || !digest.starts_with("sha256:")
                        || !digest.as_bytes()[7..]
                            .iter()
                            .all(|c| c.is_ascii_digit() || (b'a'..=b'f').contains(c))
                })
            {
                return Err(WireError);
            }
            Params::Start(p)
        }
        Method::Inspect => Params::Deployment(typed(&value)?),
        Method::Stop => Params::Stop(typed(&value)?),
        Method::OperationInspect | Method::Cancel => Params::Operation(typed(&value)?),
        Method::List => {
            let p: ListParams = typed(&value)?;
            if p.limit.is_some_and(|n| n == 0 || n > 100)
                || p.cursor.as_ref().is_some_and(String::is_empty)
            {
                return Err(WireError);
            }
            Params::List(p)
        }
        Method::Wait => Params::Wait(typed(&value)?),
        Method::Migrate => Params::Migrate(typed(&value)?),
        Method::Subscribe => {
            let p: SubscribeParams = typed(&value)?;
            if p.cursor.as_ref().is_some_and(String::is_empty) {
                return Err(WireError);
            }
            Params::Subscribe(p)
        }
        Method::Unsubscribe => Params::Unsubscribe(typed(&value)?),
    };
    Ok((request, params))
}

#[derive(Debug, Clone, Deserialize, serde::Serialize)]
#[serde(try_from = "String")]
pub struct UtcTimestamp(pub String);
impl TryFrom<String> for UtcTimestamp {
    type Error = WireError;
    fn try_from(value: String) -> Result<Self, WireError> {
        let normalized = if let Some(prefix) = value.strip_suffix("+00:00") {
            format!("{prefix}Z")
        } else {
            value
        };
        if !normalized.ends_with('Z') || humantime::parse_rfc3339(&normalized).is_err() {
            return Err(WireError);
        }
        Ok(Self(normalized))
    }
}
#[derive(Debug, Clone, Copy, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Readiness {
    Pending,
    Ready,
    Failed,
}
#[derive(Debug, Clone, Copy, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum WriterOwnership {
    Held,
    Released,
    Uncertain,
}
#[derive(Debug, Clone, Copy, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum DesiredState {
    Running,
    Stopped,
}
#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ProtocolError {
    pub code: String,
    pub stage: String,
    pub retryable: bool,
    pub message: String,
    pub operation_id: Option<Identifier>,
    pub deployment_id: Option<Identifier>,
    pub revision: Option<String>,
}
impl ProtocolError {
    pub fn exit_category(&self) -> i32 {
        match self.code.as_str() {
            "INVALID_ARGUMENT" | "NEGOTIATION_REQUIRED" | "MALFORMED_FRAME" => 2,
            "UNAUTHORIZED_PEER" => 3,
            "NOT_FOUND" => 4,
            "IDEMPOTENCY_CONFLICT"
            | "DEPLOYMENT_CONFLICT"
            | "OWNERSHIP_UNCERTAIN"
            | "MIGRATION_CONFLICT" => 5,
            "INCOMPATIBLE_VERSION"
            | "UNSUPPORTED_CAPABILITY"
            | "UNSUPPORTED_HOST"
            | "UNSAFE_CANCELLATION" => 6,
            "DEADLINE_EXCEEDED" => 7,
            "RESOURCE_LIMIT"
            | "CURSOR_EXPIRED"
            | "STREAM_OVERFLOW"
            | "PROVISIONING_REQUIRED"
            | "MIGRATION_PRECONDITION"
            | "OPERATION_FAILED" => 8,
            _ => 1,
        }
    }
}
#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Operation {
    pub operation_id: Identifier,
    pub method: Method,
    pub deployment_id: Option<Identifier>,
    pub accepted_at: UtcTimestamp,
    pub updated_at: UtcTimestamp,
    pub state: OperationState,
    pub recovery: Recovery,
    pub result: Option<Value>,
    pub error: Option<ProtocolError>,
    pub target_operation_id: Option<Identifier>,
}
