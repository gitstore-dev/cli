mod support;
use serde_json::{Value, json};
use support::{Harness, stderr};
use tiny_http::{Response, Server};

#[test]
fn writable_markdown_roundtrips_without_observed_state_or_body_normalization() {
    for kind in ["Product", "CategoryTaxonomy"] {
        for body in ["", "ü", "ü\n", "ü\n\n", "---\nbody\n---\n", "\r\nline\r\n"] {
            let resource = json!({"apiVersion":"catalog.gitstore.dev/v1beta1", "kind":kind,
                "metadata":{"name":"café","namespace":"shop","labels":{"a":"b"},"uid":"server-uid","resourceVersion":"42"},
                "spec":{"title":"Title\nUnicode ü"},"status":{"conditions":[]},"body":body});
            let markdown = gs::export::markdown(&resource).unwrap();
            let document = gs::authoring::parse_for_profile(
                &markdown,
                None,
                false,
                Some("gitstore-alpha9-2026-11-05"),
            )
            .unwrap();
            assert_eq!(document.input["body"], body);
            assert_eq!(document.input["spec"], resource["spec"]);
            assert_eq!(
                document.input["metadata"]["labels"],
                resource["metadata"]["labels"]
            );
            assert!(document.input.get("status").is_none());
            assert!(document.input["metadata"].get("uid").is_none());
            assert!(document.input["metadata"].get("resourceVersion").is_none());
        }
    }
}

#[test]
fn authoring_export_refuses_unrepresentable_fields_and_nonempty_scoped_body() {
    let mut value = json!({"apiVersion":"gitstore.dev/v1beta1", "kind":"Namespace", "metadata":{"name":"shop"}, "spec":{"tier":"USER"},"body":""});
    assert!(gs::export::markdown(&value).is_ok());
    value["body"] = json!("description");
    assert!(gs::export::markdown(&value).is_err());
    value["body"] = json!("");
    value["spec"]["pushPolicyDefaults"] =
        json!({"receivePackHooks":{"preReceive":{"enabled":true}}});
    assert!(gs::export::markdown(&value).is_err());
    value["kind"] = json!("Collection");
    assert!(gs::export::markdown(&value).is_err());
}

#[test]
fn structured_reads_and_pages_preserve_body_metadata_spec_and_status() {
    for format in ["json", "yaml", "table", "markdown"] {
        for list in [false, true] {
            if format == "markdown" && list {
                continue;
            }
            let h = Harness::new();
            let server = Server::http("127.0.0.1:0").unwrap();
            h.configure(&format!(
                "http://{}/proxy/admin/graphql",
                server.server_addr()
            ));
            let config = std::fs::read_to_string(h.config())
                .unwrap()
                .replace("m0-2026-10-27", "gitstore-alpha9-2026-11-05");
            h.write("config.toml", &config);
            let resource = json!({"id":"opaque", "apiVersion":"catalog.gitstore.dev/v1beta1", "kind":"Product",
                "metadata":{"name":"café", "namespace":"shop", "labels":{"unicode":"é"},"annotations":{"note":"line\nline"},"uid":"u","resourceVersion":"17","generation":2},
                "spec":{"title":"Café", "options":[{"name":"size","values":["large"]}]},
                "status":{"observedGeneration":1,"conditions":[{"type":"Ready","status":"False","message":"pending"}]},
                "body":"---\nUnchanged ü\n\n"});
            let page = json!({"items":[resource.clone()],"pageInfo":{"hasNextPage":true,"hasPreviousPage":false,"endCursor":"opaque-page","startCursor":"start"}});
            let expected = if list { page.clone() } else { resource.clone() };
            let worker = std::thread::spawn(move || {
                let mut request = server
                    .recv_timeout(std::time::Duration::from_secs(5))
                    .unwrap()
                    .unwrap();
                assert_eq!(request.url(), "/proxy/admin/graphql");
                let mut data = String::new();
                request.as_reader().read_to_string(&mut data).unwrap();
                let envelope: Value = serde_json::from_str(&data).unwrap();
                let query = envelope["query"].as_str().unwrap();
                assert!(
                    query.contains("status {")
                        && query.contains("options {")
                        && query.contains("ownerReferences")
                );
                let reply = if list {
                    assert_eq!(envelope["variables"]["first"], 1);
                    json!({"data":{"products":{"edges":[{"node":resource}],"pageInfo":page["pageInfo"]}}})
                } else {
                    json!({"data":{"product":resource}})
                };
                request
                    .respond(Response::from_string(reply.to_string()))
                    .unwrap();
            });
            let args = if list {
                vec!["list", "product", "--first", "1"]
            } else {
                vec!["read", "product", "café"]
            };
            let mut command = h.credential_cmd("synthetic-export-token");
            command.args(["-o", format]).args(args);
            if format == "markdown" {
                command.arg("--authoring");
            }
            let out = command.output().unwrap();
            worker.join().unwrap();
            assert!(out.status.success(), "{}", stderr(&out));
            if format == "markdown" {
                let document = gs::authoring::parse_for_profile(
                    std::str::from_utf8(&out.stdout).unwrap(),
                    None,
                    false,
                    Some("gitstore-alpha9-2026-11-05"),
                )
                .unwrap();
                assert_eq!(document.input["body"], expected["body"]);
                assert_eq!(document.input["spec"], expected["spec"]);
                assert!(document.input.get("status").is_none());
                continue;
            }
            if format == "table" {
                let text = std::str::from_utf8(&out.stdout).unwrap();
                assert!(!text.contains("Unchanged"));
                assert!(!text.contains("pending"));
                assert!(!text.contains("observedGeneration"));
                continue;
            }
            let actual: Value = if format == "json" {
                serde_json::from_slice(&out.stdout).unwrap()
            } else {
                serde_saphyr::from_str(std::str::from_utf8(&out.stdout).unwrap()).unwrap()
            };
            assert_eq!(actual, expected);
            assert!(out.stderr.is_empty());
        }
    }
}
