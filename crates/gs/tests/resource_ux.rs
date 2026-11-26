//! Resource selection spellings, not-found diagnostics, Ready display and
//! help for incomplete commands, exercised through the real CLI process.
mod support;
use support::*;
use tiny_http::{Response, Server};

/// Serve one canned reply; return the request body the CLI sent.
fn serve_once(
    h: &Harness,
    reply: &'static str,
) -> (std::thread::JoinHandle<serde_json::Value>, String) {
    let server = Server::http("127.0.0.1:0").unwrap();
    h.configure(&format!("http://{}/graphql", server.server_addr()));
    let token = h.write("token", "synthetic-only");
    let worker = std::thread::spawn(move || {
        let mut request = server
            .recv_timeout(std::time::Duration::from_secs(5))
            .unwrap()
            .expect("request");
        let mut body = String::new();
        request.as_reader().read_to_string(&mut body).unwrap();
        request.respond(Response::from_string(reply)).unwrap();
        serde_json::from_str(&body).unwrap()
    });
    (worker, token.to_str().unwrap().to_owned())
}

const REPOSITORY: &str = r#"{"data":{"repository":{"apiVersion":"gitstore.dev/v1beta1","kind":"Repository","id":"r1","metadata":{"name":"docs","namespace":"shop"},"spec":{},"status":{"conditions":[]}}}}"#;

#[test]
fn kind_slash_name_and_separate_arguments_send_the_same_request() {
    let mut sent = Vec::new();
    for args in [
        vec!["read", "repository/docs"],
        vec!["read", "repository", "docs"],
    ] {
        let h = Harness::new();
        let (worker, token) = serve_once(&h, REPOSITORY);
        let mut full = vec!["--credential-file", token.as_str()];
        full.extend(args);
        full.extend(["-o", "table"]);
        let o = h.run(&full);
        sent.push(worker.join().unwrap());
        assert!(o.status.success(), "{}", stderr(&o));
        assert!(stdout(&o).contains("Name:      docs"), "{}", stdout(&o));
    }
    assert_eq!(sent[0]["variables"], sent[1]["variables"]);
    assert_eq!(
        sent[0]["variables"]["by"]["namespacePath"],
        serde_json::json!({"namespace": "shop", "name": "docs"})
    );
}

#[test]
fn ambiguous_selector_fails_before_any_request() {
    let h = Harness::new();
    h.configure("http://127.0.0.1:9/graphql");
    for args in [
        vec!["read", "repository/docs", "other"],
        vec!["read", "repository/docs", "--id", "x"],
        vec!["read", "repository/a/b"],
        vec!["delete", "repository/"],
    ] {
        let o = h.run(&args);
        assert_eq!(o.status.code(), Some(2), "{args:?}: {}", stderr(&o));
    }
}

#[test]
fn missing_resource_says_what_was_looked_up_and_where_the_namespace_came_from() {
    let h = Harness::new();
    let (worker, token) = serve_once(&h, r#"{"data":{"repository":null}}"#);
    let o = h.run(&[
        "--credential-file",
        &token,
        "read",
        "repository/docs",
        "-o",
        "table",
    ]);
    worker.join().unwrap();
    assert_eq!(o.status.code(), Some(4));
    assert!(
        stderr(&o).contains(
            "repository \"docs\" was not found in namespace \"shop\" (selected by context \"one\")"
        ),
        "{}",
        stderr(&o)
    );
}

#[test]
fn namespace_flag_is_named_as_the_origin() {
    let h = Harness::new();
    let (worker, token) = serve_once(&h, r#"{"data":{"repository":null}}"#);
    let o = h.run(&[
        "--credential-file",
        &token,
        "--namespace",
        "elsewhere",
        "read",
        "repository",
        "docs",
        "-o",
        "table",
    ]);
    worker.join().unwrap();
    assert_eq!(o.status.code(), Some(4));
    assert!(
        stderr(&o).contains("namespace \"elsewhere\" (selected by --namespace)"),
        "{}",
        stderr(&o)
    );
}

#[test]
fn listing_in_a_missing_namespace_reports_the_namespace() {
    let h = Harness::new();
    let (worker, token) = serve_once(
        &h,
        r#"{"data":null,"errors":[{"message":"x","extensions":{"code":"NOT_FOUND"}}]}"#,
    );
    let o = h.run(&[
        "--credential-file",
        &token,
        "list",
        "repository",
        "-o",
        "table",
    ]);
    worker.join().unwrap();
    assert_eq!(o.status.code(), Some(4));
    assert!(
        stderr(&o).contains("namespace \"shop\" was not found (selected by context \"one\")"),
        "{}",
        stderr(&o)
    );
}

#[test]
fn table_distinguishes_empty_conditions_from_a_ready_condition() {
    for (reply, expected) in [
        (REPOSITORY, "Ready:     none reported (no conditions yet)"),
        (
            r#"{"data":{"repository":{"apiVersion":"v1","kind":"Repository","id":"r1","metadata":{"name":"docs","namespace":"shop"},"spec":{},"status":{"conditions":[{"type":"Ready","status":"True"}]}}}}"#,
            "Ready:     True",
        ),
    ] {
        let h = Harness::new();
        let (worker, token) = serve_once(&h, reply);
        let o = h.run(&[
            "--credential-file",
            &token,
            "read",
            "repository/docs",
            "-o",
            "table",
        ]);
        worker.join().unwrap();
        assert!(o.status.success(), "{}", stderr(&o));
        assert!(stdout(&o).contains(expected), "{}", stdout(&o));
    }
}

/// A bare invocation: no injected `--config`, so clap sees no arguments at all.
fn bare(h: &Harness, args: &[&str]) -> std::process::Output {
    std::process::Command::new(env!("CARGO_BIN_EXE_gs"))
        .env_clear()
        .env("HOME", h.dir.path())
        .current_dir(h.dir.path())
        .args(args)
        .output()
        .unwrap()
}

#[test]
fn incomplete_commands_print_help_and_fail() {
    let h = Harness::new();
    for (args, expect) in [
        (vec![], "Commands:"),
        (vec!["auth"], "Commands:"),
        (vec!["repository"], "Commands:"),
        (vec!["read"], "<KIND>"),
        (vec!["list"], "<KIND>"),
        (vec!["delete"], "<KIND>"),
    ] {
        let o = bare(&h, &args);
        assert_eq!(o.status.code(), Some(2), "{args:?}");
        assert!(stdout(&o).is_empty(), "{args:?}: help must not use stdout");
        assert!(stderr(&o).contains("Usage:"), "{args:?}: {}", stderr(&o));
        assert!(stderr(&o).contains(expect), "{args:?}: {}", stderr(&o));
    }
}

#[test]
fn incomplete_commands_with_other_flags_still_fail_with_usage() {
    let h = Harness::new();
    for args in [vec![], vec!["auth"], vec!["read"], vec!["list"]] {
        let o = h.run(&args);
        assert_eq!(o.status.code(), Some(2), "{args:?}");
        assert!(stdout(&o).is_empty(), "{args:?}");
        assert!(stderr(&o).contains("Usage:"), "{args:?}: {}", stderr(&o));
    }
}

#[test]
fn missing_name_lists_the_accepted_spellings() {
    let h = Harness::new();
    h.configure("http://127.0.0.1:9/graphql");
    let o = h.run(&["read", "repository"]);
    assert_eq!(o.status.code(), Some(2));
    let e = stderr(&o);
    assert!(
        e.contains("gs read repository <name>") && e.contains("gs read repository/<name>"),
        "{e}"
    );
}

#[test]
fn machine_output_keeps_the_generic_json_diagnostic_for_incomplete_commands() {
    let h = Harness::new();
    let o = h.run(&["-o", "json", "read"]);
    assert_eq!(o.status.code(), Some(2));
    let line = stderr(&o);
    let diagnostic: serde_json::Value = serde_json::from_str(line.lines().last().unwrap()).unwrap();
    assert_eq!(diagnostic["code"], "INVALID_INPUT");
}
