use crate::{
    cli::GraphqlArgs,
    credentials::Secret,
    error::{Error, Result},
    input,
};
use cynic_parser::common::OperationType;
use serde_json::{Map, Value, json};
use std::{
    io::Read,
    time::{Duration, Instant, SystemTime},
};
pub struct Request {
    pub envelope: Value,
    pub read_only: bool,
}
pub struct Reply {
    pub body: Value,
    pub error: Option<Error>,
}
pub fn prepare(a: &GraphqlArgs) -> Result<Request> {
    let envelope = if let Some(path) = &a.input {
        input::json(&input::read(path)?)?
    } else {
        let mut fields = Map::new();
        let mut total = 0;
        for (typed, s) in a
            .fields
            .iter()
            .map(|s| (false, s))
            .chain(a.typed.iter().map(|s| (true, s)))
        {
            total += s.len();
            if total > input::LIMIT {
                return Err(Error::usage("GraphQL input exceeds 16 MiB"));
            }
            let (key, val) = s
                .split_once('=')
                .ok_or_else(|| Error::usage("GraphQL fields require key=value"))?;
            if key.is_empty()
                || !key.chars().enumerate().all(|(i, c)| {
                    c == '_' || c.is_ascii_alphabetic() || (i > 0 && c.is_ascii_digit())
                })
                || fields.contains_key(key)
            {
                return Err(Error::usage("GraphQL keys must be nonempty and unique"));
            }
            let value = if typed {
                if let Some(p) = val.strip_prefix('@') {
                    let text = input::read(p)?;
                    total += text.len();
                    Value::String(text)
                } else {
                    match val {
                        "true" => json!(true),
                        "false" => json!(false),
                        "null" => Value::Null,
                        _ => match val.parse::<i64>() {
                            Ok(n) if n.unsigned_abs() <= 9_007_199_254_740_991 => json!(n),
                            _ => json!(val),
                        },
                    }
                }
            } else {
                json!(val)
            };
            fields.insert(key.into(), value);
        }
        if total > input::LIMIT {
            return Err(Error::usage("GraphQL input exceeds 16 MiB"));
        }
        let query = fields
            .remove("query")
            .ok_or_else(|| Error::usage("GraphQL query is required"))?;
        let name = fields.remove("operationName");
        let mut e = json!({"query":query,"variables":fields});
        if let Some(n) = name {
            e["operationName"] = n
        }
        e
    };
    validate(envelope)
}
pub fn validate(envelope: Value) -> Result<Request> {
    let obj = envelope
        .as_object()
        .ok_or_else(|| Error::usage("GraphQL envelope must be an object"))?;
    if obj
        .keys()
        .any(|k| !matches!(k.as_str(), "query" | "variables" | "operationName"))
    {
        return Err(Error::usage("Unsupported GraphQL envelope field"));
    }
    let query = obj
        .get("query")
        .and_then(Value::as_str)
        .filter(|q| !q.trim().is_empty())
        .ok_or_else(|| Error::usage("Query must be nonempty text"))?;
    if obj.get("variables").is_some_and(|v| !v.is_object()) {
        return Err(Error::usage("GraphQL variables must be an object"));
    }
    let name = match obj.get("operationName") {
        Some(Value::String(s)) if !s.is_empty() => Some(s.as_str()),
        None => None,
        _ => return Err(Error::usage("operationName must be nonempty text")),
    };
    let doc = cynic_parser::parse_executable_document(query)
        .map_err(|_| Error::usage("Invalid GraphQL document"))?;
    let ops: Vec<_> = doc.operations().collect();
    if ops.is_empty() || (ops.len() > 1 && name.is_none()) {
        return Err(Error::usage(
            "Select operationName for a multi-operation document",
        ));
    }
    let mut names = std::collections::HashSet::new();
    for o in &ops {
        if let Some(n) = o.name() {
            if !names.insert(n) {
                return Err(Error::usage("Operation names must be unique"));
            }
        } else if ops.len() > 1 {
            return Err(Error::usage(
                "Anonymous operation must be the only operation",
            ));
        }
    }
    let selected = if let Some(n) = name {
        ops.iter()
            .find(|o| o.name() == Some(n))
            .ok_or_else(|| Error::usage("operationName not found"))?
    } else {
        &ops[0]
    };
    if selected.operation_type() == OperationType::Subscription {
        return Err(Error::unsupported("DEP-017"));
    }
    let read_only = selected.operation_type() == OperationType::Query;
    Ok(Request {
        envelope,
        read_only,
    })
}
pub struct Client {
    http: reqwest::blocking::Client,
    endpoint: String,
    token: Secret,
    deadline: Instant,
}
impl Client {
    pub fn new(endpoint: &str, token: Secret, timeout: Duration) -> Result<Self> {
        let http = reqwest::blocking::Client::builder()
            .redirect(reqwest::redirect::Policy::none())
            .connect_timeout(timeout.min(Duration::from_secs(5)))
            .build()
            .map_err(|_| Error::remote("Cannot initialize HTTP client"))?;
        Ok(Self {
            http,
            endpoint: endpoint.into(),
            token,
            deadline: Instant::now() + timeout,
        })
    }
    pub fn execute(&self, request: &Request) -> Result<Reply> {
        self.execute_inner(request).map_err(|mut error| {
            error.stage = "transport";
            if !request.read_only && !matches!(error.exit, 2..=6) {
                error.uncertain()
            } else {
                error
            }
        })
    }
    fn execute_inner(&self, request: &Request) -> Result<Reply> {
        for attempt in 0..3 {
            let remaining = self
                .deadline
                .checked_duration_since(Instant::now())
                .ok_or_else(Error::deadline)?;
            let sent = self
                .http
                .post(&self.endpoint)
                .bearer_auth(self.token.expose())
                .timeout(remaining)
                .json(&request.envelope)
                .send();
            let mut response = match sent {
                Ok(r) => r,
                Err(e) => {
                    if e.is_timeout() {
                        return Err(if request.read_only {
                            Error::deadline()
                        } else {
                            Error::deadline().uncertain()
                        });
                    }
                    if request.read_only && attempt < 2 {
                        self.pause(Duration::from_millis(100))?;
                        continue;
                    }
                    return Err(if request.read_only {
                        Error::remote("HTTP connection failed")
                    } else {
                        Error::remote("Mutation response unavailable; inspect before retrying")
                            .uncertain()
                    });
                }
            };
            let status = response.status().as_u16();
            if request.read_only && attempt < 2 && (status == 429 || matches!(status, 502..=504)) {
                let delay = response
                    .headers()
                    .get("retry-after")
                    .and_then(|v| v.to_str().ok())
                    .and_then(|s| {
                        s.parse::<u64>().map(Duration::from_secs).ok().or_else(|| {
                            httpdate::parse_http_date(s)
                                .ok()
                                .map(|d| d.duration_since(SystemTime::now()).unwrap_or_default())
                        })
                    })
                    .unwrap_or(Duration::from_millis(100));
                self.pause(delay)?;
                continue;
            }
            if !(200..300).contains(&status) {
                return Err(match status {
                    401 | 403 => Error::auth("Server denied authentication or authorization"),
                    404 => Error::new(4, "NOT_FOUND", "Public target not found"),
                    409 => Error::new(5, "CONFLICT", "Server reported a conflict"),
                    _ => Error::remote("HTTP request failed"),
                });
            }
            let mut bytes = Vec::new();
            response
                .by_ref()
                .take((input::LIMIT + 1) as u64)
                .read_to_end(&mut bytes)
                .map_err(|_| {
                    if request.read_only {
                        Error::remote("Cannot read response")
                    } else {
                        Error::remote("Mutation response interrupted").uncertain()
                    }
                })?;
            if bytes.len() > input::LIMIT {
                return Err(Error::remote("Response exceeds 16 MiB"));
            }
            let body = input::json(
                std::str::from_utf8(&bytes).map_err(|_| Error::remote("Response is not UTF-8"))?,
            )
            .map_err(|_| Error::remote("Response is malformed JSON"))?;
            let has_errors = body
                .get("errors")
                .and_then(Value::as_array)
                .is_some_and(|errors| !errors.is_empty());
            if !body.is_object()
                || body
                    .get("data")
                    .is_some_and(|v| !v.is_object() && !v.is_null())
                || (!body
                    .get("data")
                    .is_some_and(|v| v.is_object() || v.is_null())
                    && !has_errors)
            {
                return Err(Error::remote("Invalid GraphQL response envelope"));
            }
            if body.get("errors").is_some_and(|v| !v.is_array()) {
                return Err(Error::remote("GraphQL errors must be an array"));
            }
            let error = if body
                .get("errors")
                .and_then(Value::as_array)
                .is_some_and(|e| !e.is_empty())
            {
                let e = response_error(&body);
                Some(if request.read_only { e } else { e.uncertain() })
            } else {
                None
            };
            return Ok(Reply { body, error });
        }
        unreachable!()
    }
    fn pause(&self, delay: Duration) -> Result<()> {
        let left = self
            .deadline
            .checked_duration_since(Instant::now())
            .ok_or_else(Error::deadline)?;
        if delay >= left {
            return Err(Error::deadline());
        }
        std::thread::sleep(delay);
        Ok(())
    }
}
fn response_error(body: &Value) -> Error {
    let codes: Vec<_> = body["errors"]
        .as_array()
        .into_iter()
        .flatten()
        .filter_map(|e| e["extensions"]["code"].as_str())
        .collect();
    let mut error = if codes
        .iter()
        .any(|c| matches!(*c, "FORBIDDEN" | "UNAUTHORIZED" | "UNAUTHENTICATED"))
    {
        Error::auth("GraphQL authentication or authorization failed")
    } else if codes.contains(&"NOT_FOUND") {
        Error::new(4, "NOT_FOUND", "GraphQL target not found")
    } else if codes
        .iter()
        .any(|c| matches!(*c, "CONFLICT" | "ALREADY_EXISTS"))
    {
        Error::new(5, "CONFLICT", "GraphQL conflict")
    } else if codes
        .iter()
        .any(|c| matches!(*c, "GRAPHQL_VALIDATION_FAILED" | "GRAPHQL_PARSE_FAILED"))
    {
        Error::unsupported("selected GraphQL contract incompatible with server")
    } else {
        Error::remote("GraphQL returned operation errors; inspect response data")
    };
    error.stage = "graphql";
    error
}
