use gs::daemon_protocol::{FRAME_MAX, ITEMS_MAX, STRING_MAX, decode, read_frame, write_frame};
use serde_json::{Value, json};
use std::{
    io::{self, Cursor, Read},
    path::Path,
};

fn rejects(s: &str) {
    assert!(decode(s.as_bytes()).is_err(), "accepted: {s}");
}
#[test]
fn strict_json_keys_numbers_strings_and_depth() {
    for s in [
        "[]",
        "null",
        "1",
        "{} {}",
        r#"{"n":1.0}"#,
        r#"{"n":1e0}"#,
        r#"{"n":9007199254740992}"#,
        r#"{"n":-9007199254740992}"#,
        r#"{"x":1,"\u0078":2}"#,
        r#"{"x":NaN}"#,
        r#"{"a\u0000b":1,"a\u0000b":2}"#,
    ] {
        rejects(s);
    }
    for n in [-9_007_199_254_740_991i64, 9_007_199_254_740_991] {
        assert!(decode(format!("{{\"n\":{n}}}").as_bytes()).is_ok());
    }
    assert!(decode(br#"{"a\u0000b":"hello\u0000world"}"#).is_ok());
    assert!(decode(b"{\"s\":\"\xff\"}").is_err());
    for depth in [32, 33] {
        let s = format!(
            "{{\"a\":{}0{}}}",
            "[".repeat(depth - 1),
            "]".repeat(depth - 1)
        );
        assert_eq!(decode(s.as_bytes()).is_ok(), depth == 32);
    }
    assert!(
        decode(
            serde_json::to_string(&json!({"s":"x".repeat(STRING_MAX)}))
                .unwrap()
                .as_bytes()
        )
        .is_ok()
    );
    assert!(
        decode(
            serde_json::to_string(&json!({"s":"x".repeat(STRING_MAX+1)}))
                .unwrap()
                .as_bytes()
        )
        .is_err()
    );
    assert!(
        decode(
            serde_json::to_string(&json!({"a":vec![0;ITEMS_MAX]}))
                .unwrap()
                .as_bytes()
        )
        .is_ok()
    );
    assert!(
        decode(
            serde_json::to_string(&json!({"a":vec![0;ITEMS_MAX+1]}))
                .unwrap()
                .as_bytes()
        )
        .is_err()
    );
}
struct Fragmented(Cursor<Vec<u8>>);
impl Read for Fragmented {
    fn read(&mut self, b: &mut [u8]) -> io::Result<usize> {
        let len = b.len().min(1);
        self.0.read(&mut b[..len])
    }
}
#[test]
fn fragmented_coalesced_and_incomplete_frames() {
    let mut bytes = Vec::new();
    write_frame(&mut bytes, &json!({"a":1})).unwrap();
    write_frame(&mut bytes, &json!({"a":2})).unwrap();
    let mut input = Fragmented(Cursor::new(bytes));
    assert_eq!(read_frame(&mut input).unwrap(), Some(json!({"a":1})));
    assert_eq!(read_frame(&mut input).unwrap(), Some(json!({"a":2})));
    assert!(read_frame(&mut input).unwrap().is_none());
    for bytes in [
        vec![0],
        vec![0, 0, 0],
        vec![0, 0, 0, 0],
        vec![0, 0, 0, 2, b'{'],
        ((FRAME_MAX + 1) as u32).to_be_bytes().to_vec(),
    ] {
        assert!(read_frame(&mut Cursor::new(bytes)).is_err());
    }
}
#[test]
fn canonical_m0_examples_are_valid_bounded_messages() {
    let path =
        Path::new(env!("CARGO_MANIFEST_DIR")).join("../../tests/fixtures/daemon/examples.json");
    let examples: Vec<Value> = serde_json::from_slice(&std::fs::read(path).unwrap()).unwrap();
    assert_eq!(
        examples
            .iter()
            .filter_map(|v| v["example"].as_str())
            .collect::<std::collections::BTreeSet<_>>()
            .len(),
        22
    );
    for v in examples {
        let bytes = serde_json::to_vec(&v["message"]).unwrap();
        assert!(
            decode(&bytes).is_ok(),
            "{} part {}",
            v["example"],
            v["part"]
        );
    }
}

#[test]
#[ignore = "requires explicitly built C wire probe; run scripts/test-daemon.sh"]
fn c_and_rust_agree_on_wire_examples_and_adversarial_inputs() {
    use std::{
        io::Write,
        process::{Command, Stdio},
    };
    let binary = std::env::var_os("GITSTORED_TEST_BINARY").expect("explicit daemon binary");
    let probe = Path::new(&binary).parent().unwrap().join("wire_probe");
    let path =
        Path::new(env!("CARGO_MANIFEST_DIR")).join("../../tests/fixtures/daemon/examples.json");
    let examples: Vec<Value> = serde_json::from_slice(&std::fs::read(path).unwrap()).unwrap();
    let mut inputs: Vec<Vec<u8>> = examples
        .into_iter()
        .map(|v| serde_json::to_vec(&v["message"]).unwrap())
        .collect();
    inputs.extend(
        approval_and_owned_stop_vectors()
            .into_iter()
            .map(|(value, _)| serde_json::to_vec(&value).unwrap()),
    );
    for s in [
        "[]",
        "null",
        "{} {}",
        r#"{"x":1,"\u0078":2}"#,
        r#"{"n":1.0}"#,
        r#"{"n":1e0}"#,
        r#"{"n":9007199254740992}"#,
        r#"{"a\u0000b":1}"#,
    ] {
        inputs.push(s.as_bytes().to_vec());
    }
    inputs.push(b"{\"s\":\"\xff\"}".to_vec());
    for depth in [32, 33] {
        inputs.push(
            format!(
                "{{\"a\":{}0{}}}",
                "[".repeat(depth - 1),
                "]".repeat(depth - 1)
            )
            .into_bytes(),
        );
    }
    for input in inputs {
        let accepted = decode(&input).is_ok();
        let mut child = Command::new(&probe)
            .env_clear()
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .spawn()
            .unwrap();
        let mut stdin = child.stdin.take().unwrap();
        stdin
            .write_all(&(input.len() as u32).to_be_bytes())
            .unwrap();
        stdin.write_all(&input).unwrap();
        drop(stdin);
        let result = child.wait_with_output().unwrap();
        if let Ok(v) = decode(&input)
            && v.get("method").is_some()
        {
            let mut typed = Command::new(&probe)
                .arg("--request")
                .env_clear()
                .stdin(Stdio::piped())
                .stdout(Stdio::piped())
                .spawn()
                .unwrap();
            let mut stdin = typed.stdin.take().unwrap();
            stdin
                .write_all(&(input.len() as u32).to_be_bytes())
                .unwrap();
            stdin.write_all(&input).unwrap();
            drop(stdin);
            assert_eq!(
                typed.wait_with_output().unwrap().status.success(),
                gs::daemon_protocol::request(&v).is_ok()
            );
        }
        assert_eq!(
            result.status.success(),
            accepted,
            "{}",
            String::from_utf8_lossy(&input)
        );
    }
}

#[test]
fn canonical_request_parameters_and_mutation_requirements() {
    use gs::daemon_protocol::request;
    let path =
        Path::new(env!("CARGO_MANIFEST_DIR")).join("../../tests/fixtures/daemon/examples.json");
    let examples: Vec<Value> = serde_json::from_slice(&std::fs::read(path).unwrap()).unwrap();
    for example in examples {
        let value = &example["message"];
        if value.get("method").is_some() {
            assert!(request(value).is_ok(), "{}", example["example"]);
        }
    }
    let good = json!({"version":{"major":1,"minor":0},"requestId":"one","method":"deployment.list","params":{}});
    assert!(request(&good).is_ok());
    for params in [
        json!({"unknown":true}),
        json!({"limit":0}),
        json!({"limit":101}),
        json!({"cursor":""}),
        json!({"cursor":null}),
    ] {
        let mut bad = good.clone();
        bad["params"] = params;
        assert!(request(&bad).is_err());
    }
    for optional in ["idempotencyKey", "timeoutMs"] {
        let mut bad = good.clone();
        bad[optional] = Value::Null;
        assert!(request(&bad).is_err());
    }
    let mut bad = good.clone();
    bad["method"] = json!("deployment.stop");
    bad["params"] = json!({"deploymentId":"local"});
    assert!(request(&bad).is_err());
    bad["idempotencyKey"] = json!("same-key");
    assert!(request(&bad).is_ok());
}

fn approval_and_owned_stop_vectors() -> Vec<(Value, bool)> {
    let start = json!({"version":{"major":1,"minor":0},"requestId":"approved","method":"deployment.start","idempotencyKey":"key","params":{"deploymentId":"one","profileId":"core","profileVersion":"1","assetManifestDigest":format!("sha256:{}","a".repeat(64))}});
    let stop = json!({"version":{"major":1,"minor":0},"requestId":"stop","method":"deployment.stop","idempotencyKey":"key","params":{"deploymentId":"one","expectedOperationId":"op-current"}});
    let mut vectors = vec![(start.clone(), true), (stop.clone(), true)];
    for digest in [
        json!("short"),
        json!(format!("sha256:{}", "A".repeat(64))),
        json!(format!("sha256:{}g", "a".repeat(63))),
        json!(null),
    ] {
        let mut value = start.clone();
        value["params"]["assetManifestDigest"] = digest;
        vectors.push((value, false));
    }
    for identity in [json!("../foreign"), json!(""), json!(null)] {
        let mut value = stop.clone();
        value["params"]["expectedOperationId"] = identity;
        vectors.push((value, false));
    }
    let mut inspect = stop;
    inspect["method"] = json!("deployment.inspect");
    vectors.push((inspect, false));
    vectors
}
#[test]
fn immutable_approval_and_owned_stop_are_strictly_typed() {
    for (value, valid) in approval_and_owned_stop_vectors() {
        assert_eq!(gs::daemon_protocol::request(&value).is_ok(), valid);
    }
}
