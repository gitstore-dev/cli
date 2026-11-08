//! Writable Markdown projection; never serialize controller status as author intent.
use crate::{
    authoring,
    error::{Error, Result},
};
use serde_json::{Value, json};
use std::io::Write;

pub fn markdown(resource: &Value) -> Result<String> {
    let kind = resource["kind"]
        .as_str()
        .ok_or_else(|| Error::remote("Missing resource kind"))?;
    if !matches!(
        kind,
        "Namespace" | "Repository" | "Product" | "CategoryTaxonomy"
    ) {
        return Err(Error::unsupported(
            "No writable authoring projection for this kind",
        ));
    }
    let mut metadata = json!({});
    for field in ["name", "namespace", "labels", "annotations"] {
        if !resource["metadata"][field].is_null() {
            metadata[field] = resource["metadata"][field].clone();
        }
    }
    let mut spec = resource["spec"].clone();
    // These reference fields describe resolved identity, not writable input.
    for reference in ["categoryRef", "parentRef"] {
        if let Some(object) = spec.get_mut(reference).and_then(Value::as_object_mut) {
            for observed in ["uid", "resourceVersion", "fieldPath"] {
                object.remove(observed);
            }
        }
    }
    let body = match resource["body"].as_str() {
        Some(body) => body,
        None if matches!(kind, "Namespace" | "Repository") && resource["body"].is_null() => "",
        _ => {
            return Err(Error::unsupported(
                "A null or non-text body cannot round-trip through Markdown",
            ));
        }
    };
    let front = json!({"apiVersion": resource["apiVersion"], "kind": kind,
                       "metadata": metadata, "spec": spec});
    let initial = format!(
        "---\n{}---\n{}",
        serde_saphyr::to_string(&front)
            .map_err(|_| Error::remote("Cannot serialize authoring projection"))?,
        body
    );
    let document =
        authoring::parse_for_profile(&initial, None, false, Some("gitstore-alpha9-2026-11-05"))
            .map_err(|_| {
                Error::unsupported(
                    "Read resource contains data outside the writable Markdown projection",
                )
            })?;
    let mut writable = document.input;
    writable
        .as_object_mut()
        .ok_or_else(|| Error::remote("Invalid authoring projection"))?
        .remove("body");
    let text = format!(
        "---\n{}---\n{}",
        serde_saphyr::to_string(&writable)
            .map_err(|_| Error::remote("Cannot serialize authoring projection"))?,
        body
    );
    if text.len() > crate::input::LIMIT {
        return Err(Error::usage("Authoring export exceeds 16 MiB"));
    }
    Ok(text)
}

pub fn emit(resource: &Value) -> Result<()> {
    std::io::stdout()
        .lock()
        .write_all(markdown(resource)?.as_bytes())
        .map_err(|_| Error::new(1, "OUTPUT", "Cannot write authoring export"))
}
