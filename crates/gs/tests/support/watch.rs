use serde_json::{Value, json};
use std::{
    net::{TcpListener, TcpStream},
    time::{Duration, Instant},
};
use tungstenite::{
    Message, WebSocket,
    handshake::server::{Callback, ErrorResponse, Request, Response},
};

pub fn listener() -> (TcpListener, String) {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    listener.set_nonblocking(true).unwrap();
    let url = format!(
        "http://{}/proxy/admin/graphql",
        listener.local_addr().unwrap()
    );
    (listener, url)
}
struct Protocol;
impl Callback for Protocol {
    fn on_request(
        self,
        request: &Request,
        mut response: Response,
    ) -> std::result::Result<Response, ErrorResponse> {
        assert_eq!(request.uri().path(), "/proxy/admin/graphql");
        assert!(request.uri().query().is_none());
        assert_eq!(
            request.headers()["sec-websocket-protocol"],
            "graphql-transport-ws"
        );
        assert_eq!(
            request.headers()["authorization"],
            "Bearer synthetic-watch-token"
        );
        response.headers_mut().insert(
            "sec-websocket-protocol",
            tungstenite::http::HeaderValue::from_static("graphql-transport-ws"),
        );
        Ok(response)
    }
}
pub fn accept(listener: &TcpListener) -> WebSocket<TcpStream> {
    let deadline = Instant::now() + Duration::from_secs(5);
    let stream = loop {
        match listener.accept() {
            Ok((stream, _)) => break stream,
            Err(error)
                if error.kind() == std::io::ErrorKind::WouldBlock && Instant::now() < deadline =>
            {
                std::thread::sleep(Duration::from_millis(5))
            }
            _ => panic!("No controlled WebSocket connection within fixture deadline"),
        }
    };
    stream.set_nonblocking(false).unwrap();
    stream
        .set_read_timeout(Some(Duration::from_secs(4)))
        .unwrap();
    stream
        .set_write_timeout(Some(Duration::from_secs(4)))
        .unwrap();
    tungstenite::accept_hdr(stream, Protocol).unwrap()
}
pub fn read(socket: &mut WebSocket<TcpStream>) -> Value {
    let message = socket.read().unwrap();
    serde_json::from_str(message.to_text().unwrap()).unwrap()
}
pub fn send(socket: &mut WebSocket<TcpStream>, value: Value) {
    socket.send(Message::text(value.to_string())).unwrap();
}
pub fn start(socket: &mut WebSocket<TcpStream>) -> Value {
    let init = read(socket);
    assert_eq!(init["type"], "connection_init");
    assert_eq!(
        init["payload"]["Authorization"],
        "Bearer synthetic-watch-token"
    );
    send(socket, json!({"type":"connection_ack"}));
    let subscription = read(socket);
    assert_eq!(subscription["type"], "subscribe");
    assert_eq!(subscription["id"], "gs-watch");
    subscription
}
pub fn event(kind: &str, event_type: &str, cursor: &str) -> Value {
    let (field, object_kind) = match kind {
        "namespace" => ("watchNamespaces", "Namespace"),
        "repository" => ("watchRepositories", "Repository"),
        "product" => ("watchProducts", "Product"),
        "category" => ("watchCategories", "CategoryTaxonomy"),
        "file" => ("watchFiles", "File"),
        _ => panic!("unsupported fixture kind"),
    };
    let object = if matches!(event_type, "ADDED" | "MODIFIED") {
        let spec = match kind {
            "namespace" => {
                json!({"title":"Fixture","tier":"USER","repositoryDefaults":null,"pushPolicyDefaults":null})
            }
            "repository" => {
                json!({"defaultBranch":"main","visibility":"PRIVATE","pushPolicy":{"maxPackSizeBytes":1000000,"maxFileSizeBytes":100000,"receivePackHooks":null,"schemaValidation":null,"admissionControl":null}})
            }
            "product" => {
                json!({"title":"Fixture","categoryRef":null,"tags":[],"media":[],"options":[],"lifecycle":{"state":"ACTIVE"}})
            }
            "category" => json!({"title":"Fixture","parentRef":null,"media":[]}),
            "file" => {
                json!({"contentType":"image/png","type":null,"provider":"http","source":{"uri":"https://example.invalid/fixture","checksum":null,"credentialsRef":null},"processing":null})
            }
            _ => unreachable!(),
        };
        json!({"id":"opaque-object-id","kind":object_kind,"apiVersion":if matches!(kind,"namespace"|"repository"){"gitstore.dev/v1beta1"}else{"catalog.gitstore.dev/v1beta1"},
            "metadata":{"name":"fixture","namespace":if kind=="namespace"{Value::Null}else{json!("shop")},"uid":"u","resourceVersion":"object-17","generation":1,"labels":{"app":"sample"},"annotations":{},"creationTimestamp":"2026-11-08T12:00:00Z","revision":null,"ownerReferences":[],"owner":{"kind":"USER","name":"alice"},"finalizers":[],"deletionTimestamp":null},
            "spec":spec,"status":null,"body":"Unchanged ü\n\n"})
    } else {
        Value::Null
    };
    let event = json!({"type":event_type,"name":if event_type=="BOOKMARK"{""}else{"fixture"},
        "namespace":if event_type=="BOOKMARK"||kind=="namespace"{Value::Null}else{json!("shop")},"resourceVersion":cursor,kind:object});
    json!({"type":"next","id":"gs-watch","payload":{"data":{field:event}}})
}
