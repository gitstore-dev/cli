mod support;
use serde_json::Value;
use support::{Harness, stderr};

#[cfg(unix)]
fn pty_resource_list(term: &str, no_color: bool, columns: &str) -> String {
    use std::{fs::File, io::Read, os::fd::FromRawFd, process::Stdio};
    use tiny_http::{Response, Server};
    let h = Harness::new();
    let server = Server::http("127.0.0.1:0").unwrap();
    h.configure(&format!("http://{}/graphql", server.server_addr()));
    let worker = std::thread::spawn(move || {
        let request = server
            .recv_timeout(std::time::Duration::from_secs(5))
            .unwrap()
            .unwrap();
        let body = serde_json::json!({"data":{"products":{"edges":[{"node":{"apiVersion":"catalog.gitstore.dev/v1beta1","kind":"Product","metadata":{"name":"hat","namespace":"shop"},"spec":{"title":"A hat"}}}],"pageInfo":{"hasNextPage":true,"hasPreviousPage":false,"startCursor":"a","endCursor":"b"}}}});
        request
            .respond(Response::from_string(body.to_string()))
            .unwrap();
    });
    let mut master = -1;
    let mut slave = -1;
    let mut size = libc::winsize {
        ws_row: 24,
        ws_col: columns.parse().unwrap(),
        ws_xpixel: 0,
        ws_ypixel: 0,
    };
    assert_eq!(
        unsafe {
            libc::openpty(
                &mut master,
                &mut slave,
                std::ptr::null_mut(),
                std::ptr::null_mut(),
                std::ptr::addr_of_mut!(size),
            )
        },
        0
    );
    let mut master = unsafe { File::from_raw_fd(master) };
    let slave = unsafe { File::from_raw_fd(slave) };
    let reader = std::thread::spawn(move || {
        let mut bytes = Vec::new();
        let _ = master.read_to_end(&mut bytes);
        bytes
    });
    let mut cmd = h.credential_cmd("synthetic-token");
    cmd.env("TERM", term)
        .env("COLUMNS", columns)
        .stdout(Stdio::from(slave))
        .args(["list", "product", "--first", "1", "-o", "table"]);
    if no_color {
        cmd.env("NO_COLOR", "1");
    }
    cmd.stderr(Stdio::piped());
    let child = cmd.spawn().unwrap();
    drop(cmd);
    let output = child.wait_with_output().unwrap();
    worker.join().unwrap();
    assert!(output.status.success(), "{}", stderr(&output));
    let bytes = reader.join().unwrap();
    String::from_utf8(bytes).unwrap().replace("\r\n", "\n")
}

#[cfg(unix)]
#[test]
fn terminal_table_color_plain_and_narrow_golden() {
    let colored = pty_resource_list("xterm-256color", false, "80");
    assert_eq!(
        colored,
        "\x1b[1mRESOURCES\x1b[0m\nKIND             NAMESPACE        NAME                 TITLE\nProduct          shop             hat                  A hat\nPage: 1 item(s); has next: true\n"
    );
    let plain = pty_resource_list("xterm-256color", true, "80");
    assert_eq!(plain, colored.replace("\x1b[1m", "").replace("\x1b[0m", ""));
    assert_eq!(pty_resource_list("dumb", false, "80"), plain);
    assert_eq!(
        pty_resource_list("xterm", true, "40"),
        "RESOURCES\nNAME                 KIND\nhat                  Product            \nPage: 1 item(s); has next: true\n"
    );
}

#[test]
fn local_context_json_has_stable_envelope() {
    let h = Harness::new();
    h.configure("https://example.test/graphql");
    let result = h.run(&["context", "show", "-o", "json"]);
    assert!(result.status.success(), "{}", stderr(&result));
    let text = std::str::from_utf8(&result.stdout).unwrap();
    assert!(text.starts_with(
        "{\n  \"apiVersion\": \"cli.gitstore.dev/v2\",\n  \"kind\": \"ContextResult\","
    ));
    let json: Value = serde_json::from_str(text).unwrap();
    assert_eq!(json["data"]["account"], "alice");
    assert_eq!(json["data"]["sources"]["account"], "context");
}

#[test]
fn redirected_table_is_plain_with_no_color_and_dumb_terminal() {
    let h = Harness::new();
    let output = h
        .cmd()
        .env("NO_COLOR", "1")
        .env("TERM", "dumb")
        .env("COLUMNS", "40")
        .args(["api-resources", "-o", "table"])
        .output()
        .unwrap();
    assert!(output.status.success(), "{}", stderr(&output));
    assert!(!output.stdout.contains(&0x1b));
    assert!(
        std::str::from_utf8(&output.stdout)
            .unwrap()
            .lines()
            .all(|line| line.len() <= 40)
    );
}
