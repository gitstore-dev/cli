//! Failed resource commands: raw GraphQL responses stay on stdout for machine
//! formats and partial data, and human formats get the diagnostic alone.
mod support;
use support::*;
use tiny_http::{Response, Server};

fn run_against(reply: &'static str, args: &[&str]) -> std::process::Output {
    let h = Harness::new();
    let server = Server::http("127.0.0.1:0").unwrap();
    h.configure(&format!("http://{}/graphql", server.server_addr()));
    let token = h.write("token", "synthetic-only");
    let worker = std::thread::spawn(move || {
        let request = server
            .recv_timeout(std::time::Duration::from_secs(5))
            .unwrap()
            .expect("request");
        request.respond(Response::from_string(reply)).unwrap();
    });
    let mut full = vec!["--credential-file", token.to_str().unwrap()];
    full.extend_from_slice(args);
    let output = h.run(&full);
    worker.join().unwrap();
    output
}

const NOT_FOUND: &str = r#"{"data":{"repository":null},"errors":[{"message":"repository not found","extensions":{"code":"NOT_FOUND"}}]}"#;
const PARTIAL: &str = r#"{"data":{"repository":{"kind":"Repository","metadata":{"name":"docs"}}},"errors":[{"message":"later field failed","extensions":{"code":"INTERNAL"}}]}"#;

const UNCLASSIFIED: &str =
    r#"{"data":null,"errors":[{"message":"boom","extensions":{"code":"INTERNAL"}}]}"#;

#[test]
fn table_output_shows_only_the_diagnostic_when_there_is_no_data() {
    for args in [
        vec!["read", "repository", "docs", "-o", "table"],
        vec!["delete", "repository", "docs", "-o", "table"],
    ] {
        let o = run_against(NOT_FOUND, &args);
        assert_eq!(o.status.code(), Some(4), "{args:?}: {}", stderr(&o));
        assert!(stdout(&o).is_empty(), "{args:?}: {}", stdout(&o));
        assert!(
            stderr(&o).contains("was not found"),
            "{args:?}: {}",
            stderr(&o)
        );
    }
}

#[test]
fn unexplained_errors_point_to_the_raw_response() {
    let o = run_against(UNCLASSIFIED, &["read", "repository", "docs", "-o", "table"]);
    assert_eq!(o.status.code(), Some(8), "{}", stderr(&o));
    assert!(stdout(&o).is_empty(), "{}", stdout(&o));
    assert!(
        stderr(&o).contains("use -o json for the server response"),
        "{}",
        stderr(&o)
    );
}

#[test]
fn json_output_keeps_the_raw_response_on_stdout() {
    let o = run_against(NOT_FOUND, &["read", "repository", "docs", "-o", "json"]);
    assert_eq!(o.status.code(), Some(4), "{}", stderr(&o));
    let body: serde_json::Value = serde_json::from_str(stdout(&o).trim()).unwrap();
    assert_eq!(body["errors"][0]["extensions"]["code"], "NOT_FOUND");
    assert!(!stderr(&o).contains("use -o json"), "{}", stderr(&o));
}

#[test]
fn partial_data_is_never_dropped_in_human_formats() {
    let o = run_against(PARTIAL, &["read", "repository", "docs", "-o", "table"]);
    assert_eq!(o.status.code(), Some(8), "{}", stderr(&o));
    assert!(stdout(&o).contains("docs"), "stdout: {}", stdout(&o));
    assert!(!stderr(&o).contains("use -o json"), "{}", stderr(&o));
}

#[test]
fn direct_graphql_keeps_the_raw_response() {
    let o = run_against(NOT_FOUND, &["api", "graphql", "-f", "query={repository}"]);
    assert!(!o.status.success());
    assert!(stdout(&o).contains("NOT_FOUND"), "stdout: {}", stdout(&o));
}
