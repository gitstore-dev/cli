mod support;
use support::*;
use tiny_http::{Header, Response, Server};
#[test]
fn literal_and_typed_variables_reach_server() {
    let h = Harness::new();
    let server = Server::http("127.0.0.1:0").unwrap();
    h.configure(&format!("http://{}/graphql", server.server_addr()));
    let token = h.write("token", "synthetic-only");
    let worker = std::thread::spawn(move || {
        let mut r = server
            .recv_timeout(std::time::Duration::from_secs(5))
            .unwrap()
            .expect("request");
        let mut b = String::new();
        r.as_reader().read_to_string(&mut b).unwrap();
        let v: serde_json::Value = serde_json::from_str(&b).unwrap();
        assert_eq!(v["variables"]["literal"], "3");
        assert_eq!(v["variables"]["number"], 3);
        r.respond(
            Response::from_string("{\"data\":{\"ok\":true},\"extensions\":{\"trace\":1}}")
                .with_header(Header::from_bytes("content-type", "application/json").unwrap()),
        )
        .unwrap();
    });
    let o = h.run(&[
        "--credential-file",
        token.to_str().unwrap(),
        "api",
        "graphql",
        "-f",
        "query=query($literal:String,$number:Int){ok}",
        "-f",
        "literal=3",
        "-F",
        "number=3",
    ]);
    worker.join().unwrap();
    assert!(o.status.success(), "{}", stderr(&o));
    assert!(stdout(&o).contains("extensions"));
}
#[test]
fn partial_errors_preserved_and_nonzero() {
    let h = Harness::new();
    let server = Server::http("127.0.0.1:0").unwrap();
    h.configure(&format!("http://{}/graphql", server.server_addr()));
    let p = h.write("token", "synthetic-only");
    let worker = std::thread::spawn(move || {
        let r = server
            .recv_timeout(std::time::Duration::from_secs(5))
            .unwrap()
            .unwrap();
        r.respond(Response::from_string(r#"{"data":{"ok":null},"errors":[{"message":"not safe to log","extensions":{"code":"FORBIDDEN"}}],"extensions":{"x":1}}"#)).unwrap()
    });
    let o = h.run(&[
        "--credential-file",
        p.to_str().unwrap(),
        "api",
        "graphql",
        "-f",
        "query={ok}",
    ]);
    worker.join().unwrap();
    assert_eq!(o.status.code(), Some(3));
    assert!(stdout(&o).contains("extensions"));
    assert!(!stderr(&o).contains("not safe to log"));
}
#[test]
fn grammar_rejects_duplicate_and_subscription_without_request() {
    let h = Harness::new();
    h.configure("http://127.0.0.1:1/graphql");
    let p = h.write("token", "synthetic-only");
    for (args, exit) in [
        (vec!["-f", "query={ok}", "-f", "x=1", "-F", "x=2"], 2),
        (vec!["-f", "query=subscription { changes }"], 6),
        (vec!["-f", "query=query A{a} query B{b}"], 2),
    ] {
        let mut c = h.cmd();
        let o = c
            .args(["--credential-file", p.to_str().unwrap(), "api", "graphql"])
            .args(args)
            .output()
            .unwrap();
        assert_eq!(o.status.code(), Some(exit), "{}", stderr(&o));
    }
}

fn response_envelope(reply: &'static str, resource: bool) -> std::process::Output {
    let h = Harness::new();
    let server = Server::http("127.0.0.1:0").unwrap();
    h.configure(&format!("http://{}/graphql", server.server_addr()));
    let token = h.write("token", "synthetic-only");
    let worker = std::thread::spawn(move || {
        let request = server
            .recv_timeout(std::time::Duration::from_secs(5))
            .unwrap()
            .unwrap();
        request.respond(Response::from_string(reply)).unwrap();
    });
    let args = if resource {
        vec!["read", "product", "fixture", "-o", "json"]
    } else {
        vec!["api", "graphql", "-f", "query={ok}"]
    };
    let output = h
        .cmd()
        .args(["--credential-file", token.to_str().unwrap()])
        .args(args)
        .output()
        .unwrap();
    worker.join().unwrap();
    output
}

#[test]
fn empty_errors_without_data_is_malformed_for_api_and_resource_reads() {
    for resource in [false, true] {
        let output = response_envelope(r#"{"errors":[]}"#, resource);
        assert_eq!(output.status.code(), Some(8));
        assert!(stdout(&output).is_empty());
        assert!(stderr(&output).contains("Invalid GraphQL response envelope"));
    }
}

#[test]
fn explicit_null_data_with_empty_errors_remains_valid() {
    let output = response_envelope(r#"{"data":null,"errors":[]}"#, false);
    assert!(output.status.success(), "{}", stderr(&output));
    assert!(stdout(&output).contains("data"));
}

#[test]
fn nonempty_errors_without_data_are_preserved() {
    let output = response_envelope(
        r#"{"errors":[{"message":"denied","extensions":{"code":"FORBIDDEN"}}],"extensions":{"trace":1}}"#,
        false,
    );
    assert_eq!(output.status.code(), Some(3));
    let body: serde_json::Value = serde_json::from_slice(&output.stdout).unwrap();
    assert_eq!(body["errors"][0]["message"], "denied");
    assert_eq!(body["extensions"]["trace"], 1);
}

#[test]
fn invalid_data_shape_is_rejected_even_with_operation_errors() {
    let output = response_envelope(r#"{"data":[],"errors":[{"message":"failed"}]}"#, false);
    assert_eq!(output.status.code(), Some(8));
    assert!(stdout(&output).is_empty());
}
