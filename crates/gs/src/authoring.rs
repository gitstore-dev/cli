use crate::{
    error::{Error, Result},
    graphql::{self, Client},
    input,
    resources::Kind,
};
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};
#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
struct Frontmatter {
    api_version: String,
    kind: String,
    metadata: Metadata,
    spec: Value,
}
#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct Metadata {
    name: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    namespace: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    labels: Option<Value>,
    #[serde(skip_serializing_if = "Option::is_none")]
    annotations: Option<Value>,
}
#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
struct Reference {
    #[serde(skip_serializing_if = "Option::is_none")]
    api_version: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    kind: Option<String>,
    name: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    namespace: Option<String>,
}
#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
struct Media {
    file_ref: FileRef,
}
#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct FileRef {
    name: String,
    kind: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    optional: Option<bool>,
}
#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct OptionDef {
    name: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    title: Option<String>,
    values: Vec<String>,
}
#[derive(Deserialize, Serialize)]
#[serde(rename_all = "SCREAMING_SNAKE_CASE")]
enum State {
    Active,
    Retired,
}
#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct Lifecycle {
    #[serde(skip_serializing_if = "Option::is_none")]
    state: Option<State>,
}
#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
struct ProductSpec {
    title: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    category_ref: Option<Reference>,
    #[serde(skip_serializing_if = "Option::is_none")]
    tags: Option<Vec<String>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    media: Option<Vec<Media>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    options: Option<Vec<OptionDef>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    lifecycle: Option<Lifecycle>,
}
#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
struct CategorySpec {
    title: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    parent_ref: Option<Reference>,
    #[serde(skip_serializing_if = "Option::is_none")]
    media: Option<Vec<Media>>,
}
pub struct Document {
    pub kind: Kind,
    pub input: Value,
}
pub fn parse(s: &str, namespace: Option<&str>) -> Result<Document> {
    parse_resolved(s, namespace, true)
}
fn parse_resolved(s: &str, namespace: Option<&str>, explicit: bool) -> Result<Document> {
    if s.len() > input::LIMIT {
        return Err(Error::usage("Input exceeds 16 MiB"));
    }
    let after_open = s
        .strip_prefix("---\r\n")
        .or_else(|| s.strip_prefix("---\n"))
        .ok_or_else(|| Error::usage("Markdown must start with YAML frontmatter delimiter"))?;
    let mut offset = 0;
    let mut end = None;
    for line in after_open.split_inclusive('\n') {
        if line.trim_end_matches(['\r', '\n']) == "---" {
            end = Some((offset, offset + line.len()));
            break;
        }
        offset += line.len();
    }
    let (end, body_start) =
        end.ok_or_else(|| Error::usage("Missing closing frontmatter delimiter"))?;
    let mut opts = serde_saphyr::Options::default();
    opts.strict_booleans = true;
    opts.reject_unsupported_tags = true;
    let mut budget = serde_saphyr::budget::Budget::default();
    budget.max_depth = 64;
    budget.max_aliases = 1000;
    budget.max_total_scalar_bytes = input::LIMIT;
    budget.max_recorded_anchor_bytes = input::LIMIT;
    budget.max_documents = 1;
    opts.budget = Some(budget);
    let mut front: Frontmatter = serde_saphyr::from_str_with_options(&after_open[..end], opts)
        .map_err(|_| {
            Error::usage("Invalid frontmatter, duplicate/unknown field or parser limit")
        })?;
    let kind = match front.kind.as_str() {
        "Product" => Kind::Product,
        "CategoryTaxonomy" => Kind::Category,
        "Namespace" | "Repository" => return Err(Error::unsupported("DEP-005")),
        "Collection" => return Err(Error::unsupported("DEP-002")),
        _ => return Err(Error::unsupported("DEP-006")),
    };
    if front.api_version != "catalog.gitstore.dev/v1beta1" || front.metadata.name.is_empty() {
        return Err(Error::usage(
            "Author document needs supported apiVersion and nonempty name",
        ));
    }
    if let Some(n) = namespace {
        if explicit
            && front
                .metadata
                .namespace
                .as_ref()
                .is_some_and(|value| value != n)
        {
            return Err(Error::usage(
                "Document namespace conflicts with selected namespace",
            ));
        }
        if front.metadata.namespace.is_none() {
            front.metadata.namespace = Some(n.into());
        }
    }
    if front
        .metadata
        .namespace
        .as_ref()
        .is_none_or(|n| n.is_empty())
    {
        return Err(Error::usage(
            "Namespaced author document requires namespace",
        ));
    }
    let spec = if kind == Kind::Product {
        let spec: ProductSpec = serde_json::from_value(front.spec)
            .map_err(|_| Error::usage("Invalid Product author spec"))?;
        if spec.title.is_empty() {
            return Err(Error::usage("Title cannot be empty"));
        }
        serde_json::to_value(spec)
    } else {
        let spec: CategorySpec = serde_json::from_value(front.spec)
            .map_err(|_| Error::usage("Invalid CategoryTaxonomy author spec"))?;
        if spec.title.is_empty() {
            return Err(Error::usage("Title cannot be empty"));
        }
        serde_json::to_value(spec)
    }
    .map_err(|_| Error::usage("Cannot project author spec"))?;
    Ok(Document {
        kind,
        input: json!({"apiVersion":front.api_version,"kind":front.kind,"metadata":front.metadata,"spec":spec,"body":&after_open[body_start..]}),
    })
}
pub fn load(files: &[String], namespace: Option<&str>, explicit: bool) -> Result<Vec<Document>> {
    if files.is_empty() || files.len() > 100 {
        return Err(Error::usage("Supply 1..100 author documents"));
    }
    let mut total = 0;
    let mut docs = vec![];
    let mut expanded = 0;
    for p in files {
        let s = input::read(p)?;
        total += s.len();
        if total > input::LIMIT {
            return Err(Error::usage("Author batch exceeds 16 MiB"));
        }
        let document = parse_resolved(&s, namespace, explicit)?;
        expanded += serde_json::to_vec(&document.input)
            .map_err(|_| Error::usage("Invalid author projection"))?
            .len();
        if expanded > input::LIMIT {
            return Err(Error::usage("Expanded author batch exceeds 16 MiB"));
        }
        docs.push(document)
    }
    Ok(docs)
}
pub fn submit(client: &Client, docs: &[Document]) -> Result<(Value, Option<Error>)> {
    let mut items = vec![];
    let mut stopped: Option<Error> = None;
    for (i, doc) in docs.iter().enumerate() {
        let mut item = json!({"index":i,"kind":doc.input["kind"],"metadata":doc.input["metadata"],"submission":"not-submitted","admission":"unverified","readiness":"unverified"});
        if stopped.is_none() {
            let field = format!("create{}", doc.kind.type_name());
            let r = graphql::validate(
                json!({"query":format!("mutation Create($input:Create{}Input!){{{field}(input:$input){{{}{{{}}}}}}}",doc.kind.type_name(),doc.kind.field(),doc.kind.fields()),"variables":{"input":doc.input}}),
            )?;
            let response = client.execute(&r);
            match response {
                Ok(reply) => {
                    let obj = &reply.body["data"][&field][doc.kind.field()];
                    if obj.is_object() {
                        item["resource"] = obj.clone();
                        if let Some(revision) = obj["metadata"]["revision"].as_str() {
                            item["revision"] = json!(revision);
                        }
                    }
                    if let Some(error) = reply.error {
                        item["submission"] = json!("unknown");
                        item["error"] = json!(error);
                        item["response"] = reply.body;
                        stopped = Some(error);
                    } else if obj.is_object() && !obj["id"].is_null() {
                        item["submission"] = json!("accepted");
                        item["admission"] = json!("succeeded");
                        item["resource"] = obj.clone();
                        if let Some(revision) = obj["metadata"]["revision"].as_str() {
                            item["revision"] = json!(revision)
                        }
                    } else {
                        let error =
                            Error::remote("Create response lacks admitted resource identity")
                                .uncertain();
                        item["submission"] = json!("unknown");
                        item["error"] = json!(error);
                        stopped = Some(error)
                    }
                }
                Err(error) => {
                    item["submission"] = json!(error.submission.as_deref().unwrap_or("unknown"));
                    item["error"] = json!(error);
                    stopped = Some(error)
                }
            }
        }
        items.push(item)
    }
    Ok((json!({"items":items}), stopped))
}
