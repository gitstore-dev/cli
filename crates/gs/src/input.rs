use crate::error::{Error, Result};
use serde::{
    Deserialize, Deserializer,
    de::{Error as _, MapAccess, SeqAccess, Visitor},
};
use serde_json::{Map, Value};
use std::{
    fmt,
    io::{self, Read},
    path::Path,
};
pub const LIMIT: usize = 16 * 1024 * 1024;
pub fn read(path: &str) -> Result<String> {
    if path == "-" {
        bounded(io::stdin().lock())
    } else {
        bounded(std::fs::File::open(path).map_err(|_| Error::usage("Cannot open input file"))?)
    }
}
pub fn bounded(r: impl Read) -> Result<String> {
    let mut b = Vec::new();
    r.take((LIMIT + 1) as u64)
        .read_to_end(&mut b)
        .map_err(|_| Error::usage("Cannot read input"))?;
    if b.len() > LIMIT {
        return Err(Error::usage("Input exceeds 16 MiB"));
    }
    String::from_utf8(b).map_err(|_| Error::usage("Input must be UTF-8"))
}
struct Unique(Value);
impl<'de> Deserialize<'de> for Unique {
    fn deserialize<D: Deserializer<'de>>(d: D) -> std::result::Result<Self, D::Error> {
        struct V;
        impl<'de> Visitor<'de> for V {
            type Value = Unique;
            fn expecting(&self, f: &mut fmt::Formatter) -> fmt::Result {
                write!(f, "JSON value without duplicate keys")
            }
            fn visit_map<A: MapAccess<'de>>(
                self,
                mut a: A,
            ) -> std::result::Result<Unique, A::Error> {
                let mut m = Map::new();
                while let Some(k) = a.next_key::<String>()? {
                    if m.contains_key(&k) {
                        return Err(A::Error::custom("duplicate key"));
                    }
                    m.insert(k, a.next_value::<Unique>()?.0);
                }
                Ok(Unique(Value::Object(m)))
            }
            fn visit_seq<A: SeqAccess<'de>>(
                self,
                mut a: A,
            ) -> std::result::Result<Unique, A::Error> {
                let mut v = vec![];
                while let Some(x) = a.next_element::<Unique>()? {
                    v.push(x.0)
                }
                Ok(Unique(Value::Array(v)))
            }
            fn visit_str<E: serde::de::Error>(self, v: &str) -> std::result::Result<Unique, E> {
                Ok(Unique(Value::String(v.into())))
            }
            fn visit_string<E: serde::de::Error>(
                self,
                v: String,
            ) -> std::result::Result<Unique, E> {
                Ok(Unique(Value::String(v)))
            }
            fn visit_bool<E: serde::de::Error>(self, v: bool) -> std::result::Result<Unique, E> {
                Ok(Unique(Value::Bool(v)))
            }
            fn visit_i64<E: serde::de::Error>(self, v: i64) -> std::result::Result<Unique, E> {
                Ok(Unique(v.into()))
            }
            fn visit_u64<E: serde::de::Error>(self, v: u64) -> std::result::Result<Unique, E> {
                Ok(Unique(v.into()))
            }
            fn visit_f64<E: serde::de::Error>(self, v: f64) -> std::result::Result<Unique, E> {
                serde_json::Number::from_f64(v)
                    .map(|n| Unique(Value::Number(n)))
                    .ok_or_else(|| E::custom("nonfinite number"))
            }
            fn visit_unit<E: serde::de::Error>(self) -> std::result::Result<Unique, E> {
                Ok(Unique(Value::Null))
            }
            fn visit_none<E: serde::de::Error>(self) -> std::result::Result<Unique, E> {
                Ok(Unique(Value::Null))
            }
        }
        d.deserialize_any(V)
    }
}
pub fn json(s: &str) -> Result<Value> {
    if s.len() > LIMIT {
        return Err(Error::usage("Input exceeds 16 MiB"));
    }
    // Validate key uniqueness first, then deserialize numbers through serde_json's
    // arbitrary-precision Value path. Its private numeric visitor representation
    // is not the wire representation and must not be retained by Unique.
    let _: Unique = serde_json::from_str(s)
        .map_err(|_| Error::usage("Invalid JSON, duplicate key or nesting limit"))?;
    let v: Value = serde_json::from_str(s).map_err(|_| Error::usage("Invalid JSON"))?;
    depth(&v, 0)?;
    Ok(v)
}
fn depth(v: &Value, n: usize) -> Result<()> {
    if n > 64 {
        return Err(Error::usage("Nesting exceeds 64 levels"));
    }
    match v {
        Value::Object(m) => {
            for x in m.values() {
                depth(x, n + 1)?
            }
        }
        Value::Array(a) => {
            for x in a {
                depth(x, n + 1)?
            }
        }
        _ => {}
    }
    Ok(())
}
pub fn protected(path: &Path) -> Result<String> {
    let mut opts = std::fs::OpenOptions::new();
    opts.read(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        opts.custom_flags(libc::O_NOFOLLOW | libc::O_NONBLOCK);
    }
    let f = opts
        .open(path)
        .map_err(|_| Error::auth("Cannot open protected credential file"))?;
    let meta = f
        .metadata()
        .map_err(|_| Error::auth("Cannot inspect credential file"))?;
    if !meta.is_file() {
        return Err(Error::auth("Credential input must be a regular file"));
    }
    #[cfg(unix)]
    {
        use std::os::unix::fs::MetadataExt;
        if meta.uid() != unsafe { libc::geteuid() } || meta.mode() & 0o077 != 0 {
            return Err(Error::auth(
                "Credential file must be user-owned with mode 0600 or stricter",
            ));
        }
    }
    bounded(f).map_err(|_| Error::auth("Credential file is invalid or oversized"))
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn duplicate_and_depth_rejected() {
        assert!(json("{\"x\":1,\"x\":2}").is_err());
        assert!(json(&format!("{}0{}", "[".repeat(65), "]".repeat(65))).is_err());
    }
    #[test]
    fn limit() {
        assert!(bounded(vec![b'a'; LIMIT + 1].as_slice()).is_err());
    }
}

#[cfg(test)]
mod exact_numbers {
    use super::*;
    #[test]
    fn complete_json_preserves_unusual_numeric_values() {
        let value =
            json(r#"{"large":184467440737095516160001,"decimal":0.123456789123456789123456789}"#)
                .unwrap();
        assert_eq!(value["large"].to_string(), "184467440737095516160001");
        assert_eq!(
            value["decimal"].to_string(),
            "0.123456789123456789123456789"
        );
    }
}
