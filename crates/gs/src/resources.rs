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
    let k = Kind::parse(&s.kind)?;
    if s.name.is_some() == s.id.is_some() {
        return Err(Error::usage("Select exactly one name or --id"));
    }
    let mut vars = json!({});
    let query = if k == Kind::File {
        if s.id.is_some() {
            return Err(Error::unsupported(
                "File by selector awaits upstream lifecycle specification",
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
            k.fields()
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
            k.fields()
        )
    };
    graphql::validate(json!({"query":query,"variables":vars}))
}
pub fn list_request(k: Kind, n: Option<&str>, first: u16, after: Option<&str>) -> Result<Request> {
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
        json!({"query":format!("query List($first:Int!,$after:String{param}){{{field}({arg}first:$first,after:$after){{edges{{node{{{}}}}} pageInfo{{hasNextPage hasPreviousPage startCursor endCursor}}}}}}",k.fields()),"variables":vars}),
    )
}
pub fn checked(client: &Client, r: &Request, f: Format) -> Result<Value> {
    let reply = client.execute(r)?;
    if let Some(error) = reply.error {
        output::emit(&reply.body, f)?;
        return Err(error);
    }
    Ok(reply.body)
}
pub fn read(client: &Client, s: &Selection, n: Option<&str>, f: Format) -> Result<Value> {
    let k = Kind::parse(&s.kind)?;
    let body = checked(client, &selection(s, n)?, f)?;
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
) -> Result<Value> {
    let body = checked(client, &list_request(k, n, first, after)?, f)?;
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
    Ok(
        json!({"items":edges.iter().map(|e|e["node"].clone()).collect::<Vec<_>>(),"pageInfo":data["pageInfo"]}),
    )
}
pub fn delete(client: &Client, s: &Selection, n: Option<&str>, f: Format) -> Result<Value> {
    let k = Kind::parse(&s.kind)?;
    k.can_delete()?;
    let obj = read(client, s, n, f)?;
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
