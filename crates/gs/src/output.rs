use crate::{
    cli::Format,
    error::{Error, Result},
};
use serde::Serialize;
use serde::ser::{SerializeMap, SerializeSeq, Serializer};
use serde_json::{Value, json};
use std::io::{self, IsTerminal, Write};

#[derive(Serialize)]
struct Envelope {
    #[serde(rename = "apiVersion")]
    api_version: &'static str,
    kind: &'static str,
    summary: Value,
    data: Value,
}

#[derive(Serialize)]
struct YamlEnvelope<'a> {
    #[serde(rename = "apiVersion")]
    api_version: &'static str,
    kind: &'static str,
    #[serde(serialize_with = "serialize_value")]
    summary: &'a Value,
    #[serde(serialize_with = "serialize_value")]
    data: &'a Value,
}

struct PlainValue<'a>(&'a Value);

fn serialize_value<S: Serializer>(
    value: &Value,
    serializer: S,
) -> std::result::Result<S::Ok, S::Error> {
    PlainValue(value).serialize(serializer)
}

impl Serialize for PlainValue<'_> {
    fn serialize<S: Serializer>(&self, serializer: S) -> std::result::Result<S::Ok, S::Error> {
        match self.0 {
            Value::Null => serializer.serialize_unit(),
            Value::Bool(v) => serializer.serialize_bool(*v),
            Value::String(v) => serializer.serialize_str(v),
            Value::Number(v) => {
                if let Some(n) = v.as_i64() {
                    serializer.serialize_i64(n)
                } else if let Some(n) = v.as_u64() {
                    serializer.serialize_u64(n)
                } else if let Some(n) = v.as_f64().filter(|n| {
                    serde_json::Number::from_f64(*n)
                        .is_some_and(|roundtrip| roundtrip.to_string() == v.to_string())
                }) {
                    serializer.serialize_f64(n)
                } else {
                    Err(serde::ser::Error::custom("number cannot be represented"))
                }
            }
            Value::Array(values) => {
                let mut seq = serializer.serialize_seq(Some(values.len()))?;
                for value in values {
                    seq.serialize_element(&PlainValue(value))?;
                }
                seq.end()
            }
            Value::Object(values) => {
                let mut map = serializer.serialize_map(Some(values.len()))?;
                for (key, value) in values {
                    map.serialize_entry(key, &PlainValue(value))?;
                }
                map.end()
            }
        }
    }
}

/// Readiness for the table view. Distinguishes "the server sent no status",
/// "the server reported no conditions yet" and "conditions exist but none is Ready"
/// from a real Ready condition, which is shown with its status and reason.
fn ready_cell(resource: &Value) -> String {
    let Some(status) = resource.get("status").filter(|s| s.is_object()) else {
        return "- (status not returned)".into();
    };
    let conditions = status["conditions"]
        .as_array()
        .map_or(&[][..], Vec::as_slice);
    if conditions.is_empty() {
        return "none reported (no conditions yet)".into();
    }
    match conditions.iter().find(|c| c["type"] == "Ready") {
        Some(ready) => {
            let state = ready["status"].as_str().unwrap_or("unknown").to_owned();
            match ready["reason"].as_str().filter(|r| !r.is_empty()) {
                Some(reason) => format!("{state} ({reason})"),
                None => state,
            }
        }
        None => "none reported (no Ready condition)".into(),
    }
}
pub fn emit(v: &Value, f: Format) -> Result<()> {
    emit_kind(
        v,
        f,
        if v.get("operationId").is_some() {
            "OperationResult"
        } else {
            "DeploymentResult"
        },
    )
}

pub fn emit_kind(v: &Value, f: Format, kind: &'static str) -> Result<()> {
    let text = render(v, f, kind)?;
    io::stdout()
        .lock()
        .write_all(text.as_bytes())
        .map_err(|_| Error::new(1, "OUTPUT", "Cannot write stdout"))
}

fn render(v: &Value, f: Format, kind: &'static str) -> Result<String> {
    let envelope = envelope(v, kind);
    Ok(match f {
        Format::Json => {
            serde_json::to_string_pretty(&envelope)
                .map_err(|_| Error::new(1, "OUTPUT", "Cannot serialize result"))?
                + "\n"
        }
        Format::Yaml => serde_saphyr::to_string(&YamlEnvelope {
            api_version: envelope.api_version,
            kind,
            summary: &envelope.summary,
            data: &envelope.data,
        })
        .map_err(|_| Error::new(1, "OUTPUT", "Cannot serialize summary"))?,
        _ => table(v),
    })
}

fn envelope(v: &Value, kind: &'static str) -> Envelope {
    let (data, summary) = match kind {
        "ResourceRead" => (
            json!({"resource": v}),
            json!({"name": v["metadata"]["name"], "namespace": v["metadata"]["namespace"], "kind": v["kind"]}),
        ),
        "ResourceList" => (
            v.clone(),
            json!({"count": v["items"].as_array().map_or(0, Vec::len)}),
        ),
        _ => {
            let mut summary = serde_json::Map::new();
            for key in [
                "name",
                "id",
                "deploymentId",
                "operationId",
                "state",
                "status",
                "submission",
                "readiness",
                "outcome",
            ] {
                if let Some(value) = v.get(key) {
                    summary.insert(key.to_owned(), value.clone());
                }
            }
            if let Some(items) = v
                .as_array()
                .or_else(|| v.get("items").and_then(Value::as_array))
            {
                summary.insert("count".into(), json!(items.len()));
            }
            (v.clone(), Value::Object(summary))
        }
    };
    Envelope {
        api_version: "cli.gitstore.dev/v2",
        kind,
        summary,
        data,
    }
}
pub fn emit_raw(v: &Value, f: Format) -> Result<()> {
    let text = match f {
        Format::Json => {
            serde_json::to_string(v)
                .map_err(|_| Error::new(1, "OUTPUT", "Cannot serialize result"))?
                + "\n"
        }
        Format::Yaml => serde_saphyr::to_string(v)
            .map_err(|_| Error::new(1, "OUTPUT", "Cannot serialize result"))?,
        _ => table(v),
    };
    io::stdout()
        .lock()
        .write_all(text.as_bytes())
        .map_err(|_| Error::new(1, "OUTPUT", "Cannot write stdout"))
}
fn table(v: &Value) -> String {
    let width = std::env::var("COLUMNS")
        .ok()
        .and_then(|s| s.parse::<usize>().ok())
        .unwrap_or(80)
        .clamp(40, 240);
    let color = io::stdout().is_terminal()
        && std::env::var_os("NO_COLOR").is_none()
        && std::env::var("TERM").is_ok_and(|term| term != "dumb");
    table_with(v, width, color)
}

fn table_with(v: &Value, width: usize, color: bool) -> String {
    fn cell(v: &Value) -> String {
        if v.is_null() {
            return "-".into();
        }
        let s = if let Some(s) = v.as_str() {
            s.to_owned()
        } else {
            v.to_string()
        };
        s.chars()
            .flat_map(|c| {
                if c.is_control() || !c.is_ascii() {
                    c.escape_default().collect::<String>()
                } else {
                    c.to_string()
                }
                .chars()
                .collect::<Vec<_>>()
            })
            .collect()
    }
    match v {
        Value::Array(items) => {
            if items.is_empty() {
                return "No results\n".into();
            }
            let mut text = format!("RESULTS ({})\n", items.len());
            for item in items {
                text.push_str(&table_with(item, width, color));
            }
            text
        }
        Value::Object(_)
            if v["metadata"].is_object()
                && v["spec"].is_object()
                && v["apiVersion"].is_string() =>
        {
            let ready = ready_cell(v);
            format!(
                "RESOURCE\n  Kind:      {}\n  Namespace: {}\n  Name:      {}\n  Title:     {}\n  Ready:     {}\n",
                cell(&v["kind"]),
                cell(&v["metadata"]["namespace"]),
                cell(&v["metadata"]["name"]),
                cell(&v["spec"]["title"]),
                ready
            )
        }
        Value::Object(_) if v["items"].is_array() && v["pageInfo"].is_object() => {
            let mut text = if color {
                "\x1b[1mRESOURCES\x1b[0m\n".to_owned()
            } else {
                "RESOURCES\n".to_owned()
            };
            let compact = width < 70;
            if compact {
                text.push_str("NAME                 KIND\n");
            } else {
                text.push_str("KIND             NAMESPACE        NAME                 TITLE\n");
            }
            for item in v["items"].as_array().into_iter().flatten() {
                let field = |value: &Value, width: usize| {
                    let s = cell(value);
                    let s: String = s.chars().take(width).collect();
                    format!("{s:width$}")
                };
                if compact {
                    text.push_str(&format!(
                        "{} {}\n",
                        field(&item["metadata"]["name"], 20),
                        field(&item["kind"], width.saturating_sub(21))
                    ));
                    continue;
                }
                text.push_str(&format!(
                    "{} {} {} {}\n",
                    field(&item["kind"], 16),
                    field(&item["metadata"]["namespace"], 16),
                    field(&item["metadata"]["name"], 20),
                    cell(&item["spec"]["title"])
                        .chars()
                        .take(width.saturating_sub(55))
                        .collect::<String>()
                ));
            }
            text.push_str(&format!(
                "Page: {} item(s); has next: {}\n",
                v["items"].as_array().map_or(0, Vec::len),
                cell(&v["pageInfo"]["hasNextPage"])
            ));
            text
        }
        Value::Object(_)
            if v["items"]
                .as_array()
                .is_some_and(|items| items.iter().all(|item| item.get("submission").is_some())) =>
        {
            let items = v["items"].as_array().expect("checked array");
            let mut text = format!("RESULTS ({})\n", items.len());
            for item in items {
                text.push_str(&format!(
                    "  {} / {}: submission={}, admission={}, readiness={}\n",
                    cell(&item["kind"]),
                    cell(&item["metadata"]["name"]),
                    cell(&item["submission"]),
                    cell(&item["admission"]),
                    cell(&item["readiness"])
                ));
            }
            text
        }
        Value::Object(obj) => obj
            .iter()
            .map(|(k, v)| {
                format!(
                    "  {}: {}\n",
                    cell(&Value::String(k.clone())),
                    cell(v)
                        .chars()
                        .take(width.saturating_sub(k.len() + 4))
                        .collect::<String>()
                )
            })
            .collect(),
        _ => format!("{}\n", cell(v)),
    }
}
pub fn diagnostic(e: &Error, machine: bool) {
    if machine {
        eprintln!(
            "{}",
            serde_json::to_string(e).unwrap_or_else(|_| "{\"code\":\"INTERNAL\"}".into())
        );
    } else {
        eprintln!("{}: {}", e.code, e.message);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn yaml_numbers_and_unicode_are_plain_values() {
        let value = json!({"generation": 2, "body": "café\n\n", "nested": [0, true]});
        let yaml = serde_saphyr::to_string(&PlainValue(&value)).unwrap();
        assert!(!yaml.contains("$serde_json::private::Number"));
        let decoded: Value = serde_saphyr::from_str(&yaml).unwrap();
        assert_eq!(decoded, value);
    }

    #[test]
    fn envelope_fields_are_ordered_and_keep_original_data() {
        let data = json!({"body":"line\n\n", "generation": 2});
        let envelope = Envelope {
            api_version: "cli.gitstore.dev/v2",
            kind: "ResourceRead",
            summary: json!({"name":"sample"}),
            data: json!({"resource":data}),
        };
        let json = serde_json::to_string_pretty(&envelope).unwrap();
        assert!(json.find("\"apiVersion\"") < json.find("\"kind\""));
        assert!(json.find("\"kind\"") < json.find("\"summary\""));
        assert!(json.find("\"summary\"") < json.find("\"data\""));
        let parsed: Value = serde_json::from_str(&json).unwrap();
        assert_eq!(parsed["data"]["resource"], data);
    }

    #[test]
    fn result_families_round_trip_across_structured_formats() {
        for kind in [
            "ResourceRead",
            "ResourceList",
            "MutationResult",
            "ContextResult",
            "ConfigResult",
            "AuthResult",
            "CatalogResult",
            "DeploymentResult",
            "OperationResult",
        ] {
            let value = if kind == "ResourceList" {
                json!({"items":[],"pageInfo":{"hasNextPage":false}})
            } else {
                json!({"metadata":{"name":"café"},"state":"unknown","body":"line\n\n","count":2})
            };
            let result = envelope(&value, kind);
            let json: Value =
                serde_json::from_str(&serde_json::to_string(&result).unwrap()).unwrap();
            let yaml = serde_saphyr::to_string(&YamlEnvelope {
                api_version: result.api_version,
                kind,
                summary: &result.summary,
                data: &result.data,
            })
            .unwrap();
            let yaml: Value = serde_saphyr::from_str(&yaml).unwrap();
            assert_eq!(json, yaml, "{kind}");
            assert_eq!(json["kind"], kind);
            assert_eq!(
                if kind == "ResourceRead" {
                    &json["data"]["resource"]
                } else {
                    &json["data"]
                },
                &value
            );
            assert!(!table_with(&value, 80, false).contains('\u{1b}'));
        }
    }

    #[test]
    fn command_family_format_goldens() {
        let input = json!({"id":"item", "state":"ready"});
        for kind in [
            "MutationResult",
            "ContextResult",
            "ConfigResult",
            "AuthResult",
            "CatalogResult",
            "DeploymentResult",
            "OperationResult",
        ] {
            let expected_json = format!(
                "{{\n  \"apiVersion\": \"cli.gitstore.dev/v2\",\n  \"kind\": \"{kind}\",\n  \"summary\": {{\n    \"id\": \"item\",\n    \"state\": \"ready\"\n  }},\n  \"data\": {{\n    \"id\": \"item\",\n    \"state\": \"ready\"\n  }}\n}}\n"
            );
            let expected_yaml = format!(
                "apiVersion: cli.gitstore.dev/v2\nkind: {kind}\nsummary:\n  id: item\n  state: ready\ndata:\n  id: item\n  state: ready\n"
            );
            assert_eq!(render(&input, Format::Json, kind).unwrap(), expected_json);
            assert_eq!(render(&input, Format::Yaml, kind).unwrap(), expected_yaml);
            assert_eq!(
                table_with(&input, 80, false),
                "  id: item\n  state: ready\n"
            );
        }
        let page = json!({"items":[],"pageInfo":{"hasNextPage":false}});
        assert_eq!(
            render(&page, Format::Json, "ResourceList").unwrap(),
            "{\n  \"apiVersion\": \"cli.gitstore.dev/v2\",\n  \"kind\": \"ResourceList\",\n  \"summary\": {\n    \"count\": 0\n  },\n  \"data\": {\n    \"items\": [],\n    \"pageInfo\": {\n      \"hasNextPage\": false\n    }\n  }\n}\n"
        );
        assert_eq!(
            render(&page, Format::Yaml, "ResourceList").unwrap(),
            "apiVersion: cli.gitstore.dev/v2\nkind: ResourceList\nsummary:\n  count: 0\ndata:\n  items: []\n  pageInfo:\n    hasNextPage: false\n"
        );
        assert_eq!(
            table_with(&page, 80, false),
            "RESOURCES\nKIND             NAMESPACE        NAME                 TITLE\nPage: 0 item(s); has next: false\n"
        );
        let resource = json!({"apiVersion":"v1","kind":"Product","metadata":{"name":"hat","namespace":"shop"},"spec":{"title":"Hat"}});
        assert_eq!(
            render(&resource, Format::Json, "ResourceRead").unwrap(),
            "{\n  \"apiVersion\": \"cli.gitstore.dev/v2\",\n  \"kind\": \"ResourceRead\",\n  \"summary\": {\n    \"kind\": \"Product\",\n    \"name\": \"hat\",\n    \"namespace\": \"shop\"\n  },\n  \"data\": {\n    \"resource\": {\n      \"apiVersion\": \"v1\",\n      \"kind\": \"Product\",\n      \"metadata\": {\n        \"name\": \"hat\",\n        \"namespace\": \"shop\"\n      },\n      \"spec\": {\n        \"title\": \"Hat\"\n      }\n    }\n  }\n}\n"
        );
        assert_eq!(
            render(&resource, Format::Yaml, "ResourceRead").unwrap(),
            "apiVersion: cli.gitstore.dev/v2\nkind: ResourceRead\nsummary:\n  kind: Product\n  name: hat\n  namespace: shop\ndata:\n  resource:\n    apiVersion: v1\n    kind: Product\n    metadata:\n      name: hat\n      namespace: shop\n    spec:\n      title: Hat\n"
        );
        assert_eq!(
            table_with(&resource, 80, false),
            "RESOURCE\n  Kind:      Product\n  Namespace: shop\n  Name:      hat\n  Title:     Hat\n  Ready:     - (status not returned)\n"
        );
    }

    #[test]
    fn resource_page_table_identifies_rows_and_pagination() {
        let page = json!({"items":[{"kind":"Product","metadata":{"namespace":"shop","name":"hat"},"spec":{"title":"Hat"}}],"pageInfo":{"hasNextPage":true}});
        let rendered = table_with(&page, 80, false);
        assert!(rendered.contains("KIND"));
        assert!(rendered.contains("shop"));
        assert!(rendered.contains("hat"));
        assert!(rendered.contains("has next: true"));
        assert!(!rendered.contains('\u{1b}'));
    }

    #[test]
    fn mutation_table_keeps_uncertain_states_visible() {
        let result = json!({"items":[{"kind":"Product","metadata":{"name":"hat"},"submission":"unknown","admission":"unverified","readiness":"unverified"}]});
        let rendered = table_with(&result, 80, false);
        assert!(rendered.contains("Product / hat"));
        assert!(rendered.contains("submission=unknown"));
        assert!(rendered.contains("admission=unverified"));
    }

    #[test]
    fn ready_cell_distinguishes_missing_status_empty_conditions_and_real_conditions() {
        let with = |status: Value| json!({"status": status});
        assert_eq!(ready_cell(&json!({})), "- (status not returned)");
        assert_eq!(
            ready_cell(&with(json!({"conditions": []}))),
            "none reported (no conditions yet)"
        );
        assert_eq!(
            ready_cell(&with(json!({}))),
            "none reported (no conditions yet)"
        );
        assert_eq!(
            ready_cell(&with(
                json!({"conditions": [{"type": "Synced", "status": "True"}]})
            )),
            "none reported (no Ready condition)"
        );
        assert_eq!(
            ready_cell(&with(
                json!({"conditions": [{"type": "Ready", "status": "True"}]})
            )),
            "True"
        );
        assert_eq!(
            ready_cell(&with(
                json!({"conditions": [{"type": "Ready", "status": "False", "reason": "Pending"}]})
            )),
            "False (Pending)"
        );
    }
}
