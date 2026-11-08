//! Local built-in field/operation knowledge; no configuration or transport.
use crate::{
    cli::{Cli, Command},
    error::{Error, Result},
    output,
    resources::Kind,
};
use serde_json::{Value, json};
use std::collections::BTreeSet;

const ALPHA9: &str = "gitstore-alpha9-2026-11-05";

fn writable_paths(
    inputs: &Value,
    name: &str,
    prefix: &str,
    output: &mut BTreeSet<String>,
    depth: usize,
) {
    if depth > 12 {
        return;
    }
    if let Some(fields) = inputs[name].as_object() {
        for (field, field_type) in fields {
            let path = if prefix.is_empty() {
                field.clone()
            } else {
                format!("{prefix}.{field}")
            };
            output.insert(path.clone());
            if let Some(field_type) = field_type.as_str() {
                writable_paths(
                    inputs,
                    field_type.trim_matches(['[', ']', '!']),
                    &path,
                    output,
                    depth + 1,
                );
            }
        }
    }
}

fn field_tree(
    tokens: &[&str],
    position: &mut usize,
    type_name: &str,
    prefix: &str,
    types: &Value,
    writable: &BTreeSet<String>,
) -> Result<Value> {
    let mut result = serde_json::Map::new();
    while *position < tokens.len() && tokens[*position] != "}" {
        let field = tokens[*position];
        *position += 1;
        let field_type = types[type_name][field].as_str().ok_or_else(|| {
            Error::new(
                1,
                "CATALOG_INVALID",
                "Built-in field evidence does not match its projection",
            )
        })?;
        let path = if prefix.is_empty() {
            field.to_owned()
        } else {
            format!("{prefix}.{field}")
        };
        let mut node = json!({"type":field_type,"writable":writable.contains(&path)});
        if tokens.get(*position) == Some(&"{") {
            *position += 1;
            node["fields"] = field_tree(
                tokens,
                position,
                field_type.trim_matches(['[', ']', '!']),
                &path,
                types,
                writable,
            )?;
            if tokens.get(*position) != Some(&"}") {
                return Err(Error::new(
                    1,
                    "CATALOG_INVALID",
                    "Unbalanced built-in field projection",
                ));
            }
            *position += 1;
        }
        result.insert(field.to_owned(), node);
    }
    Ok(Value::Object(result))
}

pub fn catalog(profile: &str) -> Result<Value> {
    if !matches!(profile, ALPHA9 | "m0-2026-10-27") {
        return Err(Error::unsupported(
            "Select a recorded built-in contract profile",
        ));
    }
    let evidence: Value = serde_json::from_str(include_str!("resource_catalog.json"))
        .map_err(|_| Error::new(1, "CATALOG_INVALID", "Cannot read built-in field evidence"))?;
    let alpha9 = profile == ALPHA9;
    let mut entries = Vec::new();
    for kind in [
        Kind::Namespace,
        Kind::Repository,
        Kind::Product,
        Kind::Category,
        Kind::Collection,
        Kind::File,
    ] {
        let create = matches!(kind, Kind::Product | Kind::Category)
            || alpha9 && matches!(kind, Kind::Namespace | Kind::Repository);
        let mut writable = BTreeSet::new();
        if create {
            writable_paths(
                &evidence["inputs"],
                &format!("Create{}Input", kind.type_name()),
                "",
                &mut writable,
                0,
            );
        }
        if kind == Kind::Namespace {
            writable.remove("metadata.namespace");
        }
        let fields = if alpha9 {
            crate::projections::fields(kind)
        } else {
            kind.fields()
        };
        let spaced = fields.replace('{', " { ").replace('}', " } ");
        let tokens: Vec<_> = spaced.split_whitespace().collect();
        let tree = field_tree(
            &tokens,
            &mut 0,
            kind.type_name(),
            "",
            &evidence["types"],
            &writable,
        )?;
        entries.push(json!({"name":kind.field(),"kind":kind.manifest(),"scope":if kind==Kind::Namespace{"cluster"}else{"namespace"},
            "operations":{"read":true,"readById":kind!=Kind::File || alpha9,"list":kind!=Kind::File,"create":create,
                "delete":!matches!(kind,Kind::Collection|Kind::File),"update":false,"watch":alpha9 && kind!=Kind::Collection,"watchById":false,
                "authoringExport":alpha9 && create},
            "readFormats":if alpha9 {vec!["table","json","yaml"]} else {vec!["table","json"]},
            "authoringLimit":"Only exact writable projections; unrepresentable body/spec data is rejected",
            "fields":tree}));
    }
    Ok(json!({"origin":"local built-in","profile":profile,
        "upstreamRevision":if alpha9 {evidence["sourceRevision"].as_str().unwrap_or("")} else {"b6c866421dbcf43994565f847553c1f6aec83994"},
        "resources":entries}))
}

pub fn dispatch(cli: &Cli) -> Result<()> {
    if cli.credential_file.is_some() || cli.credential_stdin {
        return Err(Error::usage(
            "Local resource help does not accept credential input",
        ));
    }
    let (profile, path) = match &cli.command {
        Command::ApiResources { profile } => (profile, None),
        Command::Explain { resource, profile } => (profile, Some(resource.as_str())),
        _ => return Err(Error::usage("Select api-resources or explain")),
    };
    let catalog = catalog(profile)?;
    let Some(path) = path else {
        return output::emit(&catalog, cli.format());
    };
    let mut parts = path.split('.');
    let kind = parts.next().unwrap_or("");
    let entry = catalog["resources"]
        .as_array()
        .and_then(|entries| entries.iter().find(|entry| entry["name"] == kind))
        .ok_or_else(|| Error::unsupported("Select a built-in resource kind"))?;
    let mut selected = entry.clone();
    for field in parts {
        selected = selected["fields"].get(field).cloned().ok_or_else(|| {
            Error::usage("Field is unavailable in the selected built-in projection")
        })?;
    }
    output::emit(
        &json!({"origin":"local built-in","profile":profile,"upstreamRevision":catalog["upstreamRevision"],"path":path,"definition":selected}),
        cli.format(),
    )
}
