use crate::{
    cli::Format,
    cli::Selection,
    error::{Error, Result},
    graphql::{self, Client, Request},
    output,
};
use serde_json::{Value, json};
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Kind {
    Namespace,
    Repository,
    Product,
    Category,
    Collection,
    File,
}
impl Kind {
    pub fn parse(s: &str) -> Result<Self> {
        match s {
            "namespace" => Ok(Self::Namespace),
            "repository" => Ok(Self::Repository),
            "product" => Ok(Self::Product),
            "category" => Ok(Self::Category),
            "collection" => Ok(Self::Collection),
            "file" => Ok(Self::File),
            _ => Err(Error::unsupported(
                "verified kind mapping required; DEP-006",
            )),
        }
    }
    pub fn field(self) -> &'static str {
        match self {
            Self::Namespace => "namespace",
            Self::Repository => "repository",
            Self::Product => "product",
            Self::Category => "category",
            Self::Collection => "collection",
            Self::File => "file",
        }
    }
    pub fn type_name(self) -> &'static str {
        match self {
            Self::Namespace => "Namespace",
            Self::Repository => "Repository",
            Self::Product => "Product",
            Self::Category => "Category",
            Self::Collection => "Collection",
            Self::File => "File",
        }
    }
    pub fn manifest(self) -> &'static str {
        if self == Self::Category {
            "CategoryTaxonomy"
        } else {
            self.type_name()
        }
    }
    fn plural(self) -> Result<&'static str> {
        match self {
            Self::Namespace => Ok("namespaces"),
            Self::Repository => Ok("repositories"),
            Self::Product => Ok("products"),
            Self::Category => Ok("categories"),
            Self::Collection => Ok("collections"),
            Self::File => Err(Error::unsupported("DEP-007: no File list mapping")),
        }
    }
    pub fn fields(self) -> String {
        let metadata = if self == Self::Namespace {
            "name uid resourceVersion generation revision"
        } else {
            "name namespace uid resourceVersion generation revision"
        };
        let spec = match self {
            Self::Namespace => "",
            Self::Repository => "spec { defaultBranch }",
            Self::Product | Self::Category | Self::Collection => "spec { title }",
            Self::File => "spec { contentType }",
        };
        format!("id apiVersion kind metadata {{ {metadata} }} body {spec}")
    }
    pub fn can_delete(self) -> Result<()> {
        match self {
            Self::Collection => Err(Error::unsupported("DEP-002")),
            Self::File => Err(Error::unsupported("DEP-007")),
            _ => Ok(()),
        }
    }
}
fn namespace(n: Option<&str>) -> Result<&str> {
    n.filter(|s| !s.is_empty())
        .ok_or_else(|| Error::usage("Select a namespace"))
}
pub fn selection(s: &Selection, n: Option<&str>) -> Result<Request> {
    selection_fields(s, n, &Kind::parse(&s.kind)?.fields())
}
fn selection_fields(s: &Selection, n: Option<&str>, fields: &str) -> Result<Request> {
    let k = Kind::parse(&s.kind)?;
    if s.name.is_some() == s.id.is_some() {
        return Err(Error::usage("Select exactly one name or --id"));
    }
    let mut vars = json!({});
    let query = if k == Kind::File {
        if s.id.is_some() {
            return Err(Error::unsupported(
                "File IDs require the explicit alpha.9 contract profile",
            ));
        }
        let name = s
            .name
            .as_deref()
            .filter(|s| !s.is_empty())
            .ok_or_else(|| Error::usage("Name cannot be empty"))?;
        vars = json!({"namespace":namespace(n)?,"name":name});
        format!(
            "query Read($namespace:String!,$name:String!){{file(namespace:$namespace,name:$name){{{}}}}}",
            fields
        )
    } else {
        let by = if let Some(id) = &s.id {
            if id.is_empty() {
                return Err(Error::usage("ID cannot be empty"));
            }
            json!({"id":id})
        } else {
            let name = s
                .name
                .as_deref()
                .filter(|s| !s.is_empty())
                .ok_or_else(|| Error::usage("Name cannot be empty"))?;
            if k == Kind::Namespace {
                json!({"name":name})
            } else {
                json!({"namespacePath":{"namespace":namespace(n)?,"name":name}})
            }
        };
        vars["by"] = by;
        format!(
            "query Read($by:{}By!){{{}(by:$by){{{}}}}}",
            k.type_name(),
            k.field(),
            fields
        )
    };
    graphql::validate(json!({"query":query,"variables":vars}))
}
pub fn list_request(k: Kind, n: Option<&str>, first: u16, after: Option<&str>) -> Result<Request> {
    list_request_for_profile(k, n, first, after, None)
}
pub fn list_request_for_profile(
    k: Kind,
    n: Option<&str>,
    first: u16,
    after: Option<&str>,
    profile: Option<&str>,
) -> Result<Request> {
    if !(1..=100).contains(&first) {
        return Err(Error::usage("Page size must be 1..100"));
    }
    let field = k.plural()?;
    let (param, arg) = if k == Kind::Namespace {
        ("", "")
    } else {
        (",$namespace:String!", "namespace:$namespace,")
    };
    let mut vars = json!({"first":first,"after":after});
    if k != Kind::Namespace {
        vars["namespace"] = json!(namespace(n)?)
    }
    graphql::validate(
        json!({"query":format!("query List($first:Int!,$after:String{param}){{{field}({arg}first:$first,after:$after){{edges{{node{{{}}}}} pageInfo{{hasNextPage hasPreviousPage startCursor endCursor}}}}}}",profile_fields(k, profile)),"variables":vars}),
    )
}
/// Does a GraphQL response carry any non-null top-level data?
fn has_data(body: &Value) -> bool {
    body["data"]
        .as_object()
        .is_some_and(|data| data.values().any(|value| !value.is_null()))
}
pub fn checked(client: &Client, r: &Request, f: Format) -> Result<Value> {
    let reply = client.execute(r)?;
    if let Some(mut error) = reply.error {
        // Machine formats always receive the raw response, and partial data is never
        // dropped. A human reader with nothing but errors gets the diagnostic alone;
        // the raw response is one flag away.
        if matches!(f, Format::Json | Format::Yaml) || has_data(&reply.body) {
            output::emit_raw(&reply.body, f)?;
        } else {
            error
                .message
                .push_str(" (use -o json for the server response)");
        }
        return Err(error);
    }
    Ok(reply.body)
}
pub fn selection_for_profile(
    s: &Selection,
    n: Option<&str>,
    profile: Option<&str>,
) -> Result<Request> {
    let kind = Kind::parse(&s.kind)?;
    if profile != Some("gitstore-alpha9-2026-11-05") || kind != Kind::File {
        return selection_fields(s, n, &profile_fields(kind, profile));
    }
    if s.name.is_some() == s.id.is_some() {
        return Err(Error::usage("Select exactly one name or --id"));
    }
    let by = if let Some(id) = &s.id {
        if id.is_empty() {
            return Err(Error::usage("ID cannot be empty"));
        }
        json!({"id": id})
    } else {
        let name = s
            .name
            .as_deref()
            .filter(|s| !s.is_empty())
            .ok_or_else(|| Error::usage("Name cannot be empty"))?;
        json!({"namespacePath": {"namespace": namespace(n)?, "name": name}})
    };
    graphql::validate(json!({
        "query": format!("query Read($by:FileBy!){{file(by:$by){{{}}}}}", profile_fields(kind, profile)),
        "variables": {"by": by}
    }))
}
fn profile_fields(kind: Kind, profile: Option<&str>) -> String {
    if profile == Some("gitstore-alpha9-2026-11-05") {
        crate::projections::fields(kind)
    } else {
        kind.fields()
    }
}
/// Replace a bare NOT_FOUND with what was looked up and where the namespace came from.
/// Other errors pass through. `origin` describes the namespace selection source.
pub fn explain_not_found(
    mut error: Error,
    kind: Kind,
    name: Option<&str>,
    id: Option<&str>,
    namespace: Option<&str>,
    origin: &str,
    listing: bool,
) -> Error {
    if error.code != "NOT_FOUND" {
        return error;
    }
    let kind_name = kind.field();
    let message = match (kind == Kind::Namespace, listing, name, id, namespace) {
        (_, _, None, Some(id), _) => format!("{kind_name} with id \"{id}\" was not found"),
        (true, ..) => format!(
            "namespace \"{}\" was not found. Run 'gs list namespace' to see the available namespaces",
            name.unwrap_or_default()
        ),
        (false, true, _, _, Some(ns)) => format!(
            "namespace \"{ns}\" was not found (selected by {origin}). Run 'gs list namespace' to see the available namespaces, or pass --namespace"
        ),
        (false, false, Some(name), _, Some(ns)) => format!(
            "{kind_name} \"{name}\" was not found in namespace \"{ns}\" (selected by {origin}). Check the name, or pass --namespace; 'gs list namespace' shows the available namespaces"
        ),
        _ => return error,
    };
    error.message = message;
    error
}
pub fn read(
    client: &Client,
    s: &Selection,
    n: Option<&str>,
    f: Format,
    profile: Option<&str>,
) -> Result<Value> {
    let k = Kind::parse(&s.kind)?;
    let body = checked(client, &selection_for_profile(s, n, profile)?, f)?;
    let obj = &body["data"][k.field()];
    if obj.is_null() {
        return Err(Error::new(4, "NOT_FOUND", "Selected resource not found"));
    }
    if obj["kind"] != k.manifest() {
        return Err(Error::new(
            2,
            "WRONG_KIND",
            "Server returned a different resource kind",
        ));
    }
    Ok(obj.clone())
}
pub fn list(
    client: &Client,
    k: Kind,
    n: Option<&str>,
    first: u16,
    after: Option<&str>,
    f: Format,
    profile: Option<&str>,
) -> Result<Value> {
    let body = checked(
        client,
        &list_request_for_profile(k, n, first, after, profile)?,
        f,
    )?;
    let data = &body["data"][k.plural()?];
    let edges = data["edges"]
        .as_array()
        .ok_or_else(|| Error::remote("Missing connection edges"))?;
    if edges.len() > usize::from(first) {
        return Err(Error::remote("Server exceeded requested page bound"));
    }
    if !data["pageInfo"].is_object() {
        return Err(Error::remote("Missing pageInfo"));
    }
    if edges
        .iter()
        .any(|edge| !edge["node"].is_object() || edge["node"]["kind"] != k.manifest())
    {
        return Err(Error::remote(
            "Connection returned a missing or wrong-kind node",
        ));
    }
    Ok(
        json!({"items":edges.iter().map(|e|e["node"].clone()).collect::<Vec<_>>(),"pageInfo":data["pageInfo"]}),
    )
}
pub fn delete(client: &Client, s: &Selection, n: Option<&str>, f: Format) -> Result<Value> {
    let k = Kind::parse(&s.kind)?;
    k.can_delete()?;
    let obj = read(client, s, n, f, None)?;
    let id = obj["id"]
        .as_str()
        .filter(|s| !s.is_empty())
        .ok_or_else(|| Error::remote("Lookup did not return an ID"))?;
    let field = format!("delete{}", k.type_name());
    let r = graphql::validate(
        json!({"query":format!("mutation Delete($input:Delete{}Input!){{{field}(input:$input){{outcome {}{{{}}}}}}}",k.type_name(),k.field(),k.fields()),"variables":{"input":{"id":id}}}),
    )?;
    let result = checked(client, &r, f)?;
    let p = &result["data"][&field];
    if !matches!(
        p["outcome"].as_str(),
        Some("TERMINATION_STARTED" | "ALREADY_TERMINATING")
    ) {
        return Err(Error::remote("Missing deletion outcome").uncertain());
    }
    Ok(
        json!({"id":id,"outcome":p["outcome"],"submission":"accepted","readiness":"unverified","resource":p[k.field()]}),
    )
}
#[cfg(test)]
mod not_found_tests {
    use super::*;
    fn not_found() -> Error {
        Error::new(4, "NOT_FOUND", "GraphQL target not found")
    }
    fn explain(kind: Kind, name: Option<&str>, id: Option<&str>, listing: bool) -> Error {
        explain_not_found(
            not_found(),
            kind,
            name,
            id,
            Some("catalogue"),
            "context \"development\"",
            listing,
        )
    }
    #[test]
    fn missing_resource_names_kind_name_namespace_and_origin() {
        let e = explain(Kind::Repository, Some("gitstore-system"), None, false);
        assert_eq!((e.exit, e.code), (4, "NOT_FOUND"));
        assert_eq!(
            e.message,
            "repository \"gitstore-system\" was not found in namespace \"catalogue\" (selected by context \"development\"). Check the name, or pass --namespace; 'gs list namespace' shows the available namespaces"
        );
    }
    #[test]
    fn listing_in_a_missing_namespace_says_the_namespace_is_missing() {
        let e = explain(Kind::Repository, None, None, true);
        assert!(e.message.starts_with(
            "namespace \"catalogue\" was not found (selected by context \"development\")"
        ));
        assert!(e.message.contains("gs list namespace"));
    }
    #[test]
    fn namespace_reads_and_id_lookups_do_not_blame_the_selected_namespace() {
        let e = explain(Kind::Namespace, Some("nope"), None, false);
        assert!(e.message.starts_with("namespace \"nope\" was not found"));
        assert!(!e.message.contains("catalogue"));
        let e = explain(Kind::Product, None, Some("abc"), false);
        assert_eq!(e.message, "product with id \"abc\" was not found");
    }
    #[test]
    fn other_errors_are_untouched() {
        let other = Error::new(3, "CREDENTIAL", "denied");
        let e = explain_not_found(
            other,
            Kind::Repository,
            Some("x"),
            None,
            Some("ns"),
            "flag",
            false,
        );
        assert_eq!((e.code, e.message.as_str()), ("CREDENTIAL", "denied"));
    }
}
