use crate::{
    cli::Format,
    error::{Error, Result},
};
use serde_json::Value;
use std::io::{self, Write};
pub fn emit(v: &Value, f: Format) -> Result<()> {
    let text = match f {
        Format::Json => {
            serde_json::to_string(v)
                .map_err(|_| Error::new(1, "OUTPUT", "Cannot serialize result"))?
                + "\n"
        }
        Format::Yaml => serde_saphyr::to_string(v)
            .map_err(|_| Error::new(1, "OUTPUT", "Cannot serialize summary"))?,
        _ => table(v),
    };
    io::stdout()
        .lock()
        .write_all(text.as_bytes())
        .map_err(|_| Error::new(1, "OUTPUT", "Cannot write stdout"))
}
fn table(v: &Value) -> String {
    fn cell(v: &Value) -> String {
        let s = if let Some(s) = v.as_str() {
            s.to_owned()
        } else {
            v.to_string()
        };
        s.chars().flat_map(char::escape_default).collect()
    }
    match v {
        Value::Array(items) => items.iter().map(table).collect(),
        Value::Object(obj) => obj
            .iter()
            .map(|(k, v)| format!("{}\t{}\n", cell(&Value::String(k.clone())), cell(v)))
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
