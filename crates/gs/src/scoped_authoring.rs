//! Exact writable alpha.9 Namespace and Repository input fields.
use crate::{
    error::{Error, Result},
    resources::Kind,
};
use serde::{Deserialize, Serialize};
use serde_json::Value;

#[derive(Deserialize, Serialize)]
#[serde(rename_all = "SCREAMING_SNAKE_CASE")]
enum Tier {
    User,
    Organization,
}
#[derive(Deserialize, Serialize)]
#[serde(rename_all = "SCREAMING_SNAKE_CASE")]
enum Visibility {
    Public,
    Private,
    Internal,
}
#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
struct RepositoryDefaults {
    #[serde(skip_serializing_if = "Option::is_none")]
    visibility: Option<Visibility>,
    #[serde(skip_serializing_if = "Option::is_none")]
    default_branch: Option<String>,
}
#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
struct PushDefaults {
    #[serde(skip_serializing_if = "Option::is_none")]
    max_pack_size_bytes: Option<i64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    max_file_size_bytes: Option<i64>,
}
#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
struct NamespaceSpec {
    #[serde(skip_serializing_if = "Option::is_none")]
    title: Option<String>,
    tier: Tier,
    #[serde(skip_serializing_if = "Option::is_none")]
    repository_defaults: Option<RepositoryDefaults>,
    #[serde(skip_serializing_if = "Option::is_none")]
    push_policy_defaults: Option<PushDefaults>,
}
#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
struct RepositorySpec {
    #[serde(skip_serializing_if = "Option::is_none")]
    default_branch: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    visibility: Option<Visibility>,
    #[serde(skip_serializing_if = "Option::is_none")]
    storage_class: Option<String>,
}

pub fn project(kind: Kind, value: Value) -> Result<Value> {
    let result = match kind {
        Kind::Namespace => {
            serde_json::from_value::<NamespaceSpec>(value).and_then(serde_json::to_value)
        }
        Kind::Repository => {
            serde_json::from_value::<RepositorySpec>(value).and_then(serde_json::to_value)
        }
        _ => return Err(Error::usage("Select Namespace or Repository authoring")),
    };
    result.map_err(|_| Error::usage("Invalid or unknown scoped authoring spec fields"))
}
