//! Bounded typed journal streaming over the shipped graphql-transport-ws contract.
use crate::{
    cli::{Cli, Format, WatchArgs},
    config::Effective,
    credentials,
    error::{Error, Result},
    input,
    resources::Kind,
    watch_checkpoint::{Checkpoint, Scope, Store},
};
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};
use std::{
    collections::BTreeMap,
    net::{TcpStream, ToSocketAddrs},
    time::{Duration, Instant},
};
use tungstenite::{
    Message, WebSocket, client::IntoClientRequest, handshake::HandshakeError,
    protocol::WebSocketConfig, stream::MaybeTlsStream,
};
use url::Url;

pub const MAX_QUEUED_EVENTS: usize = 1;
const MAX_RETRIES: u32 = 5;
type Socket = WebSocket<MaybeTlsStream<TcpStream>>;

fn named(code: &'static str, message: &str) -> Error {
    let mut error = Error::new(8, code, message);
    error.stage = "watch";
    error
}
fn expired() -> Error {
    Error::new(
        5,
        "WATCH_EXPIRED",
        "Journal continuity expired; explicitly resnapshot and start a new watch without the expired cursor",
    )
}
fn metadata(kind: Kind) -> Result<(&'static str, &'static str)> {
    match kind {
        Kind::Namespace => Ok(("watchNamespaces", "__namespace_watch_bootstrap__")),
        Kind::Repository => Ok(("watchRepositories", "__repository_watch_bootstrap__")),
        Kind::Product => Ok(("watchProducts", "__product_watch_bootstrap__")),
        Kind::Category => Ok(("watchCategories", "__category_watch_bootstrap__")),
        Kind::File => Ok(("watchFiles", "__file_watch_bootstrap__")),
        Kind::Collection => Err(Error::unsupported(
            "DEP-002: Collection has no public typed watch",
        )),
    }
}

#[derive(Clone, Deserialize, Serialize, PartialEq, Eq, PartialOrd, Ord)]
#[serde(rename_all = "SCREAMING_SNAKE_CASE")]
enum Operator {
    In,
    NotIn,
    Exists,
    DoesNotExist,
}
#[derive(Clone, Deserialize, Serialize, PartialEq, Eq, PartialOrd, Ord)]
#[serde(deny_unknown_fields)]
struct Requirement {
    key: String,
    operator: Operator,
    #[serde(default)]
    values: Vec<String>,
}
#[derive(Default, Deserialize, Serialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
struct Selector {
    #[serde(default)]
    match_labels: BTreeMap<String, String>,
    #[serde(default)]
    match_expressions: Vec<Requirement>,
}
fn selector(raw: Option<&str>) -> Result<Value> {
    let Some(raw) = raw else {
        return Ok(Value::Null);
    };
    if raw.len() > 16384 {
        return Err(Error::usage("Watch selector exceeds 16 KiB"));
    }
    let mut selected: Selector = serde_json::from_value(input::json(raw)?).map_err(|_| {
        Error::usage("Use a JSON LabelSelectorInput with matchLabels/matchExpressions")
    })?;
    if selected.match_labels.len() > 64
        || selected.match_expressions.len() > 64
        || selected
            .match_labels
            .iter()
            .any(|(key, value)| key.is_empty() || key.len() > 256 || value.len() > 256)
    {
        return Err(Error::usage(
            "Watch selector exceeds entry/key/value bounds",
        ));
    }
    for term in &mut selected.match_expressions {
        if term.key.is_empty()
            || term.key.len() > 256
            || term.values.len() > 64
            || term.values.iter().any(|value| value.len() > 256)
            || matches!(term.operator, Operator::In | Operator::NotIn) == term.values.is_empty()
        {
            return Err(Error::usage(
                "Invalid watch selector expression or value arity",
            ));
        }
        term.values.sort();
        term.values.dedup();
    }
    selected.match_expressions.sort();
    selected.match_expressions.dedup();
    if selected.match_labels.is_empty() && selected.match_expressions.is_empty() {
        return Ok(Value::Null);
    }
    serde_json::to_value(selected).map_err(|_| Error::usage("Cannot encode watch selector"))
}
fn matches_selector(selector: &Value, object: &Value) -> bool {
    if selector.is_null() {
        return true;
    }
    let Ok(selector) = serde_json::from_value::<Selector>(selector.clone()) else {
        return false;
    };
    let labels = &object["metadata"]["labels"];
    selector
        .match_labels
        .iter()
        .all(|(key, value)| labels[key].as_str() == Some(value))
        && selector.match_expressions.iter().all(|term| {
            let value = labels[&term.key].as_str();
            match term.operator {
                Operator::In => {
                    value.is_some_and(|value| term.values.iter().any(|item| item == value))
                }
                Operator::NotIn => {
                    value.is_none_or(|value| !term.values.iter().any(|item| item == value))
                }
                Operator::Exists => value.is_some(),
                Operator::DoesNotExist => value.is_none(),
            }
        })
}
pub fn preflight(cli: &Cli, args: &WatchArgs) -> Result<()> {
    let kind = Kind::parse(&args.kind)?;
    let (_, bootstrap) = metadata(kind)?;
    if args.id.is_some() {
        return Err(Error::unsupported(
            "DEP-018: ID watches need UID-bearing deletion events; select a name or labels",
        ));
    }
    if cli.format() != Format::Json {
        return Err(Error::usage("Typed watches emit NDJSON; use JSON output"));
    }
    if args
        .name
        .as_ref()
        .is_some_and(|name| name.is_empty() || name.len() > 4096)
        || !(1024..=16_777_216).contains(&args.max_frame_bytes)
    {
        return Err(Error::usage("Invalid watch name or frame bound"));
    }
    if let Some(cursor) = &args.resource_version
        && (cursor.is_empty()
            || cursor.len() > 65536
            || cursor != bootstrap
                && [
                    Kind::Namespace,
                    Kind::Repository,
                    Kind::Product,
                    Kind::Category,
                    Kind::File,
                ]
                .into_iter()
                .any(|kind| metadata(kind).is_ok_and(|(_, sentinel)| cursor == sentinel)))
    {
        return Err(Error::usage(
            "Select a bounded matching journal cursor or this kind's bootstrap sentinel",
        ));
    }
    selector(args.selector.as_deref())?;
    Ok(())
}
fn left(deadline: Instant) -> Result<Duration> {
    deadline
        .checked_duration_since(Instant::now())
        .filter(|left| !left.is_zero())
        .ok_or_else(Error::deadline)
}
fn socket_error(error: tungstenite::Error) -> Error {
    match error {
        tungstenite::Error::Protocol(
            tungstenite::error::ProtocolError::ResetWithoutClosingHandshake,
        ) => named(
            "WATCH_DISCONNECTED",
            "Watch connection ended without a close handshake",
        ),
        tungstenite::Error::Capacity(_) => named(
            "WATCH_FRAME_LIMIT",
            "Watch frame/message or write buffer exceeded its bound",
        ),
        tungstenite::Error::Tls(_) => named("WATCH_TLS", "WebSocket TLS verification failed"),
        tungstenite::Error::Http(response) if matches!(response.status().as_u16(), 401 | 403) => {
            Error::auth("WebSocket authentication or authorization denied")
        }
        tungstenite::Error::Http(response) if response.status().is_redirection() => {
            Error::unsupported("Select the explicit WebSocket endpoint; redirects are not followed")
        }
        tungstenite::Error::Http(response) if response.status().as_u16() == 404 => Error::new(
            4,
            "NOT_FOUND",
            "Configured WebSocket endpoint was not found",
        ),
        tungstenite::Error::Http(response)
            if response.status().as_u16() == 429 || response.status().is_server_error() =>
        {
            named(
                "WATCH_CONNECT",
                "WebSocket endpoint temporarily unavailable",
            )
        }
        tungstenite::Error::Http(_) => {
            Error::unsupported("Configured endpoint did not accept the verified WebSocket upgrade")
        }
        tungstenite::Error::Protocol(_) | tungstenite::Error::Utf8(_) => {
            named("WATCH_PROTOCOL", "Invalid WebSocket framing or protocol")
        }
        _ => named(
            "WATCH_DISCONNECTED",
            "Watch transport disconnected; retained cursor is required for reconnect",
        ),
    }
}
fn connect(endpoint: &str, token: &str, deadline: Instant, max_frame: u32) -> Result<Socket> {
    crate::initialize_http_tls();
    let mut url = Url::parse(endpoint).map_err(|_| Error::usage("Invalid watch endpoint"))?;
    let origin = url.origin().ascii_serialization();
    let scheme = if url.scheme() == "https" { "wss" } else { "ws" };
    url.set_scheme(scheme)
        .map_err(|_| Error::usage("Cannot select WebSocket endpoint"))?;
    let host = url
        .host_str()
        .ok_or_else(|| Error::usage("Missing watch host"))?
        .trim_matches(['[', ']']);
    let port = url
        .port_or_known_default()
        .ok_or_else(|| Error::usage("Missing watch port"))?;
    let addresses = (host, port)
        .to_socket_addrs()
        .map_err(|_| named("WATCH_CONNECT", "Cannot resolve watch endpoint"))?;
    let mut connected = None;
    for address in addresses.take(16) {
        if let Ok(stream) =
            TcpStream::connect_timeout(&address, left(deadline)?.min(Duration::from_secs(5)))
        {
            connected = Some(stream);
            break;
        }
    }
    let stream =
        connected.ok_or_else(|| named("WATCH_CONNECT", "Cannot connect to watch endpoint"))?;
    stream
        .set_read_timeout(Some(left(deadline)?.min(Duration::from_secs(15))))
        .and_then(|_| {
            stream.set_write_timeout(Some(
                left(deadline)
                    .unwrap_or(Duration::from_millis(1))
                    .min(Duration::from_secs(5)),
            ))
        })
        .map_err(|_| named("WATCH_CONNECT", "Cannot bound watch socket I/O"))?;
    let mut request = url
        .as_str()
        .into_client_request()
        .map_err(|_| Error::usage("Invalid WebSocket request"))?;
    request.headers_mut().insert(
        "sec-websocket-protocol",
        tungstenite::http::HeaderValue::from_static("graphql-transport-ws"),
    );
    request.headers_mut().insert(
        "origin",
        origin
            .parse()
            .map_err(|_| Error::usage("Invalid watch origin"))?,
    );
    request.headers_mut().insert(
        "authorization",
        format!("Bearer {token}")
            .parse()
            .map_err(|_| Error::auth("Invalid watch credential material"))?,
    );
    let config = WebSocketConfig::default()
        .read_buffer_size(16384)
        .write_buffer_size(0)
        .max_write_buffer_size(262144)
        .max_message_size(Some(max_frame as usize))
        .max_frame_size(Some(max_frame as usize));
    let (socket, response) =
        tungstenite::client_tls_with_config(request, stream, Some(config), None).map_err(
            |error| match error {
                HandshakeError::Failure(error) => socket_error(error),
                HandshakeError::Interrupted(_) => {
                    named("WATCH_CONNECT", "WebSocket handshake interrupted")
                }
            },
        )?;
    if response
        .headers()
        .get("sec-websocket-protocol")
        .and_then(|value| value.to_str().ok())
        != Some("graphql-transport-ws")
    {
        return Err(Error::unsupported(
            "Server did not negotiate graphql-transport-ws",
        ));
    }
    Ok(socket)
}
fn bounds(socket: &mut Socket, deadline: Instant) -> Result<()> {
    let stream = match socket.get_mut() {
        MaybeTlsStream::Plain(stream) => stream,
        MaybeTlsStream::Rustls(stream) => &mut stream.sock,
        _ => return Err(Error::unsupported("Unqualified WebSocket TLS backend")),
    };
    stream
        .set_read_timeout(Some(left(deadline)?.min(Duration::from_secs(15))))
        .and_then(|_| stream.set_write_timeout(Some(Duration::from_secs(2))))
        .map_err(|_| named("WATCH_DISCONNECTED", "Cannot bound watch I/O"))
}
fn send(socket: &mut Socket, value: Value) -> Result<()> {
    socket
        .send(Message::text(value.to_string()))
        .map_err(socket_error)
}
fn receive(socket: &mut Socket, deadline: Instant) -> Result<Value> {
    loop {
        bounds(socket, deadline)?;
        let received = match socket.read() {
            Ok(message) => message,
            Err(_) if Instant::now() >= deadline => return Err(Error::deadline()),
            Err(tungstenite::Error::Io(error))
                if matches!(
                    error.kind(),
                    std::io::ErrorKind::WouldBlock
                        | std::io::ErrorKind::TimedOut
                        | std::io::ErrorKind::Interrupted
                ) =>
            {
                // A bounded read poll is not evidence of a disconnected peer.
                // Keep the subscription/cursor and retry budget until deadline.
                continue;
            }
            Err(error) => return Err(socket_error(error)),
        };
        match received {
            Message::Text(text) => {
                return input::json(text.as_str()).map_err(|_| {
                    named("WATCH_PROTOCOL", "Malformed or duplicate-key watch message")
                });
            }
            Message::Ping(_) | Message::Pong(_) => {
                socket.flush().map_err(socket_error)?;
            }
            Message::Close(Some(frame)) if matches!(u16::from(frame.code), 4401 | 4403 | 1008) => {
                return Err(Error::auth("Watch authentication expired or was revoked"));
            }
            Message::Close(_) => {
                return Err(named("WATCH_DISCONNECTED", "Watch connection closed"));
            }
            _ => {
                return Err(named(
                    "WATCH_PROTOCOL",
                    "Watch messages must be JSON text frames",
                ));
            }
        }
    }
}
fn graphql_error(errors: &Value) -> Error {
    let Some(errors) = errors.as_array().filter(|items| !items.is_empty()) else {
        return named("WATCH_PROTOCOL", "Malformed watch error payload");
    };
    let codes: Vec<_> = errors
        .iter()
        .filter_map(|error| error["extensions"]["code"].as_str())
        .collect();
    if codes.contains(&"WATCH_EXPIRED") {
        return expired();
    }
    if codes
        .iter()
        .any(|code| matches!(*code, "UNAUTHORIZED" | "UNAUTHENTICATED" | "FORBIDDEN"))
    {
        return Error::auth("Watch authentication or authorization denied");
    }
    if codes
        .iter()
        .any(|code| matches!(*code, "GRAPHQL_VALIDATION_FAILED" | "GRAPHQL_PARSE_FAILED"))
    {
        return Error::unsupported("Selected watch contract is incompatible with server");
    }
    named(
        "WATCH_REMOTE_FAILURE",
        "Server rejected or interrupted the typed watch",
    )
}
fn write_line(value: &Value, deadline: Instant) -> Result<()> {
    let mut bytes = serde_json::to_vec(value)
        .map_err(|_| named("WATCH_OUTPUT", "Cannot encode watch event"))?;
    bytes.push(b'\n');
    let deadline = deadline.min(Instant::now() + Duration::from_secs(2));
    let mut offset = 0;
    while offset < bytes.len() {
        let remaining = deadline
            .checked_duration_since(Instant::now())
            .ok_or_else(|| {
                named(
                    "WATCH_BACKPRESSURE",
                    "Observer did not consume an event within the bounded output window",
                )
            })?;
        let mut descriptor = libc::pollfd {
            fd: libc::STDOUT_FILENO,
            events: libc::POLLOUT,
            revents: 0,
        };
        let ready = unsafe {
            libc::poll(
                &mut descriptor,
                1,
                remaining.as_millis().min(i32::MAX as u128).max(1) as i32,
            )
        };
        if ready < 0 && std::io::Error::last_os_error().kind() == std::io::ErrorKind::Interrupted {
            continue;
        }
        if ready <= 0 {
            return Err(named(
                "WATCH_BACKPRESSURE",
                "Observer did not consume an event within the bounded output window",
            ));
        }
        if descriptor.revents & (libc::POLLERR | libc::POLLHUP | libc::POLLNVAL) != 0 {
            return Err(Error::new(1, "OUTPUT", "Watch output closed"));
        }
        // At most POSIX's minimum atomic pipe-write size. No shared stdout flags
        // are changed, including when a terminal is inherited from the parent.
        let size = (bytes.len() - offset).min(512);
        let written =
            unsafe { libc::write(libc::STDOUT_FILENO, bytes[offset..].as_ptr().cast(), size) };
        if written < 0 {
            if matches!(
                std::io::Error::last_os_error().kind(),
                std::io::ErrorKind::Interrupted | std::io::ErrorKind::WouldBlock
            ) {
                continue;
            }
            return Err(Error::new(1, "OUTPUT", "Cannot write watch output"));
        }
        if written == 0 {
            return Err(Error::new(
                1,
                "OUTPUT",
                "Watch output stopped accepting bytes",
            ));
        }
        offset += written as usize;
    }
    Ok(())
}
fn subscription(kind: Kind, namespace: Option<&str>, selector: &Value, cursor: &str) -> Value {
    let (field, _) = metadata(kind).expect("preflight verified typed watch kind");
    let (parameter, argument, namespace_field) = if kind == Kind::Namespace {
        ("", "", "")
    } else {
        (",$namespace:String", "namespace:$namespace,", "namespace")
    };
    let mut variables = json!({"selector":selector,"resourceVersion":cursor});
    if kind != Kind::Namespace {
        variables["namespace"] = json!(namespace);
    }
    json!({"id":"gs-watch","type":"subscribe","payload":{"operationName":"Watch",
        "query":format!("subscription Watch($selector:LabelSelectorInput,$resourceVersion:String{parameter}){{{field}({argument}selector:$selector,resourceVersion:$resourceVersion){{type name {namespace_field} resourceVersion {}{{{}}}}}}}",kind.field(),crate::projections::fields(kind)),"variables":variables}})
}

fn stream(
    socket: &mut Socket,
    args: &WatchArgs,
    store: &Store,
    record: &mut Option<Checkpoint>,
    deadline: Instant,
    count: &mut u64,
    retries: u32,
) -> Result<bool> {
    let kind = Kind::parse(&args.kind)?;
    let (field, _) = metadata(kind)?;
    loop {
        let message = receive(socket, deadline)?;
        match message["type"].as_str() {
            Some("ping") => {
                send(socket, json!({"type":"pong"}))?;
                continue;
            }
            Some("pong") => continue,
            Some("next" | "error" | "complete") if message["id"] != "gs-watch" => {
                return Err(named("WATCH_PROTOCOL", "Unexpected subscription identity"));
            }
            Some("error") => return Err(graphql_error(&message["payload"])),
            Some("complete") => {
                return Err(named("WATCH_DISCONNECTED", "Server completed the watch"));
            }
            Some("next") => {}
            _ => return Err(named("WATCH_PROTOCOL", "Unexpected watch protocol message")),
        }
        let payload = &message["payload"];
        if payload["errors"]
            .as_array()
            .is_some_and(|items| !items.is_empty())
        {
            return Err(graphql_error(&payload["errors"]));
        }
        let event = &payload["data"][field];
        let event_type = event["type"]
            .as_str()
            .filter(|value| matches!(*value, "ADDED" | "MODIFIED" | "DELETED" | "BOOKMARK"))
            .ok_or_else(|| named("WATCH_PROTOCOL", "Unknown or missing typed event"))?;
        let cursor = event["resourceVersion"]
            .as_str()
            .filter(|value| !value.is_empty() && value.len() <= 65536)
            .ok_or_else(|| named("WATCH_PROTOCOL", "Missing or oversized journal cursor"))?;
        let name = event["name"]
            .as_str()
            .filter(|value| value.len() <= 4096 && (event_type == "BOOKMARK" || !value.is_empty()))
            .ok_or_else(|| named("WATCH_PROTOCOL", "Missing or oversized event name"))?;
        let object = &event[kind.field()];
        let data_event = matches!(event_type, "ADDED" | "MODIFIED");
        if data_event
            && (!object.is_object()
                || object["kind"] != kind.manifest()
                || object["metadata"]["name"] != name)
            || !data_event && !object.is_null()
        {
            return Err(named(
                "WATCH_SCOPE_MISMATCH",
                "Typed object kind/identity does not match the event",
            ));
        }
        if event_type != "BOOKMARK" && kind != Kind::Namespace {
            let namespace = event["namespace"]
                .as_str()
                .filter(|value| !value.is_empty())
                .ok_or_else(|| {
                    named(
                        "WATCH_SCOPE_MISMATCH",
                        "Namespaced event omitted its namespace",
                    )
                })?;
            if store
                .scope()
                .namespace
                .as_deref()
                .is_some_and(|selected| selected != namespace)
                || data_event && object["metadata"]["namespace"] != namespace
            {
                return Err(named(
                    "WATCH_SCOPE_MISMATCH",
                    "Event crossed the selected namespace",
                ));
            }
        }
        if data_event && !matches_selector(&store.scope().selector, object) {
            return Err(named(
                "WATCH_SCOPE_MISMATCH",
                "Event crossed the selected labels",
            ));
        }
        if event_type != "BOOKMARK"
            && args
                .name
                .as_deref()
                .is_some_and(|selected| selected != name)
        {
            continue;
        }
        let summary = json!({"type":event_type,"name":name,"namespace":event["namespace"],"resourceVersion":cursor});
        let next = Checkpoint {
            schema_version: 1,
            scope: store.scope().clone(),
            resource_version: cursor.into(),
            last_emitted_event: Some(summary),
            connection_state: "streaming".into(),
            retries,
        };
        write_line(
            &json!({"type":event_type,"kind":kind.manifest(),"name":name,"namespace":event["namespace"],
            "resourceVersion":cursor,"object":object,"checkpoint":next}),
            deadline,
        )?;
        // Persist after successful complete output: crash recovery may duplicate
        // delivery but must never skip an event that was not fully emitted.
        store.save(&next)?;
        *record = Some(next);
        if event_type != "BOOKMARK" {
            *count += 1;
        }
        if args
            .max_events
            .is_some_and(|maximum| *count >= u64::from(maximum))
        {
            return Ok(true);
        }
    }
}

pub fn dispatch(cli: &Cli, effective: &Effective, args: &WatchArgs) -> Result<()> {
    effective.profile()?;
    if effective.context.contract_profile.as_deref() != Some("gitstore-alpha9-2026-11-05") {
        return Err(Error::unsupported(
            "Typed watches require the alpha.9 contract profile",
        ));
    }
    let kind = Kind::parse(&args.kind)?;
    let (_, bootstrap) = metadata(kind)?;
    if kind == Kind::Namespace && cli.namespace.is_some() {
        return Err(Error::usage(
            "Namespace watch is cluster scoped; omit --namespace",
        ));
    }
    let namespace = if kind == Kind::Namespace {
        None
    } else {
        effective.context.namespace.clone()
    };
    if kind != Kind::Namespace && args.name.is_some() && namespace.is_none() {
        return Err(Error::usage("A named watch requires a namespace"));
    }
    let scope = Scope {
        application_version: env!("CARGO_PKG_VERSION").into(),
        profile: "gitstore-alpha9-2026-11-05".into(),
        identity: credentials::key(effective, "graphql"),
        kind: kind.manifest().into(),
        namespace,
        name: args.name.clone(),
        selector: selector(args.selector.as_deref())?,
    };
    let store = Store::prepare(scope, args.checkpoint_file.as_deref())?;
    let saved = store.load()?;
    let mut record = if let Some(cursor) = args
        .resource_version
        .as_deref()
        .filter(|cursor| *cursor != bootstrap)
    {
        let selected=saved.filter(|record|record.resource_version==cursor).ok_or_else(||Error::usage("Resume requires the exact cursor from the matching private checkpoint; raw unbound cursors are rejected"))?;
        if selected.connection_state == "expired" {
            return Err(expired());
        }
        Some(selected)
    } else {
        None
    };
    let deadline = Instant::now() + cli.timeout();
    let mut retries = 0;
    let mut count = 0;
    let stdin_token = if cli.credential_stdin {
        Some(credentials::token(cli, effective)?)
    } else {
        None
    };
    loop {
        left(deadline)?;
        let token = if let Some(token) = &stdin_token {
            credentials::Secret::new(token.expose().to_owned())?
        } else {
            credentials::token(cli, effective)?
        };
        if token.expose().len() > 65536 {
            return Err(Error::auth("Watch credential exceeds 64 KiB"));
        }
        let result = (|| {
            let mut socket = connect(
                &effective.context.endpoint,
                token.expose(),
                deadline,
                args.max_frame_bytes,
            )?;
            send(
                &mut socket,
                json!({"type":"connection_init","payload":{"Authorization":format!("Bearer {}",token.expose())}}),
            )?;
            loop {
                let ack = receive(&mut socket, deadline)?;
                match ack["type"].as_str() {
                    Some("connection_ack") => break,
                    Some("ping") => send(&mut socket, json!({"type":"pong"}))?,
                    Some("pong") => {}
                    _ => {
                        return Err(named(
                            "WATCH_PROTOCOL",
                            "Missing graphql-transport-ws acknowledgment",
                        ));
                    }
                }
            }
            let cursor = record
                .as_ref()
                .map(|record| record.resource_version.as_str())
                .unwrap_or(bootstrap);
            send(
                &mut socket,
                subscription(
                    kind,
                    store.scope().namespace.as_deref(),
                    &store.scope().selector,
                    cursor,
                ),
            )?;
            let result = stream(
                &mut socket,
                args,
                &store,
                &mut record,
                deadline,
                &mut count,
                retries,
            );
            if matches!(result, Ok(true)) {
                let _ = send(&mut socket, json!({"id":"gs-watch","type":"complete"}));
                let _ = socket.close(None);
            }
            result
        })();
        match result {
            Ok(true) => {
                if let Some(record) = &mut record {
                    record.connection_state = "stopped".into();
                    store.save(record)?;
                }
                return Ok(());
            }
            Ok(false) => return Err(named("WATCH_PROTOCOL", "Unexpected stream state")),
            Err(error) if matches!(error.code, "WATCH_CONNECT" | "WATCH_DISCONNECTED") => {
                if error.code == "WATCH_DISCONNECTED" && record.is_none() {
                    return Err(named(
                        "WATCH_GAP",
                        "Watch disconnected before a durable position was emitted; explicitly restart or resnapshot",
                    ));
                }
                if retries >= MAX_RETRIES {
                    return Err(named(
                        "WATCH_DISCONNECTED",
                        "Watch retry budget exhausted; resume explicitly from the matching checkpoint",
                    ));
                }
                retries += 1;
                if let Some(record) = &mut record {
                    record.connection_state = "reconnecting".into();
                    record.retries = retries;
                    store.save(record)?;
                }
                let delay = Duration::from_millis(100u64 << retries.min(4));
                if delay >= left(deadline)? {
                    return Err(Error::deadline());
                }
                std::thread::sleep(delay);
            }
            Err(error) => {
                if let Some(record) = &mut record {
                    record.connection_state = if error.code == "WATCH_EXPIRED" {
                        "expired"
                    } else {
                        "failed"
                    }
                    .into();
                    store.save(record)?;
                }
                return Err(error);
            }
        }
    }
}
