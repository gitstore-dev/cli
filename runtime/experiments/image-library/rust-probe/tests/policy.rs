use gitstore_image_library_probe::policy::{Policy, transport};
use oci_client::token_cache::RegistryOperation;
use oci_client::{
    Client, Reference,
    client::{ClientConfig, ClientProtocol},
    secrets::RegistryAuth,
};
use std::{
    io::{Read, Write},
    net::{SocketAddr, TcpListener},
    thread,
    time::{Duration, Instant},
};
use tower::ServiceExt;

fn server(response: String) -> (SocketAddr, thread::JoinHandle<Vec<Vec<u8>>>) {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let address = listener.local_addr().unwrap();
    listener.set_nonblocking(true).unwrap();
    let worker = thread::spawn(move || {
        let deadline = Instant::now() + Duration::from_secs(1);
        let mut requests = Vec::new();
        while Instant::now() < deadline {
            match listener.accept() {
                Ok((mut stream, _)) => {
                    stream
                        .set_read_timeout(Some(Duration::from_millis(200)))
                        .unwrap();
                    let mut bytes = Vec::new();
                    let mut byte = [0];
                    while bytes.len() < 8192 && !bytes.ends_with(b"\r\n\r\n") {
                        if stream.read(&mut byte).unwrap_or(0) == 0 {
                            break;
                        }
                        bytes.push(byte[0]);
                    }
                    requests.push(bytes);
                    stream.write_all(response.as_bytes()).unwrap();
                }
                Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                    thread::sleep(Duration::from_millis(5))
                }
                Err(e) => panic!("fixture accept: {e}"),
            }
        }
        requests
    });
    (address, worker)
}
fn runtime() -> tokio::runtime::Runtime {
    tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
        .unwrap()
}
#[test]
fn token_realm_is_checked_before_any_connection_or_basic_auth() {
    let (unapproved, received) =
        server("HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n".into());
    let (registry, challenge) = server(format!(
        "HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: Bearer realm=\"http://{unapproved}/token\",service=\"fixture\"\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
    ));
    runtime().block_on(async {
        let client = Client::new_with_transport(
            ClientConfig {
                protocol: ClientProtocol::Http,
                ..Default::default()
            },
            transport(Policy::fixture(registry)).unwrap(),
        );
        let reference: Reference = format!("{registry}/fixture:one").parse().unwrap();
        assert!(
            client
                .auth(
                    &reference,
                    &RegistryAuth::Basic("synthetic".into(), "fixture-only".into()),
                    RegistryOperation::Pull
                )
                .await
                .is_err()
        );
    });
    assert!(!challenge.join().unwrap().is_empty());
    assert!(
        received.join().unwrap().is_empty(),
        "unapproved token service must never receive a request"
    );
}
#[test]
fn redirect_is_checked_before_any_connection_or_credentials() {
    let (unapproved, received) =
        server("HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n".into());
    let (registry, redirected) = server(format!(
        "HTTP/1.1 307 Temporary Redirect\r\nLocation: http://{unapproved}/blob\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
    ));
    runtime().block_on(async {
        let request = http::Request::builder()
            .uri(format!("http://{registry}/blob"))
            .header("Authorization", "Bearer synthetic-only")
            .body(oci_client::transport::Body::empty())
            .unwrap();
        assert!(
            transport(Policy::fixture(registry))
                .unwrap()
                .oneshot(request)
                .await
                .is_err()
        );
    });
    let sent = redirected.join().unwrap();
    assert!(String::from_utf8_lossy(&sent[0]).contains("synthetic-only"));
    assert!(received.join().unwrap().is_empty());
}
