//! Local routing is independent of remote context and credential resolution.
use crate::{
    cli::*,
    daemon_client::{Client, ClientError},
    daemon_protocol::Identifier,
    error::{Error, Result},
    output,
};
use serde::Deserialize;
use serde_json::{Value, json};
use std::{path::PathBuf, time::Duration};

pub fn is_local(command: &Command) -> bool {
    matches!(
        command,
        Command::Serve(_)
            | Command::Deployment { .. }
            | Command::Migrate(_)
            | Command::Git { .. }
            | Command::Controller { .. }
            | Command::Oidc { .. }
            | Command::Admin { .. }
            | Command::Storefront { .. }
            | Command::Api {
                command: ApiCommand::Serve(_) | ApiCommand::Config { .. }
            }
    )
}
fn component(command: &Command) -> Option<&ComponentCommand> {
    match command {
        Command::Git { command }
        | Command::Controller { command }
        | Command::Oidc { command }
        | Command::Admin { command }
        | Command::Storefront { command } => Some(command),
        _ => None,
    }
}
fn selection(cli: &Cli) -> Option<&LocalSelection> {
    match &cli.command {
        Command::Serve(a)
        | Command::Api {
            command: ApiCommand::Serve(a),
        } => Some(&a.selection),
        Command::Api {
            command:
                ApiCommand::Config {
                    command: LocalConfigCommand::Validate(a),
                },
        } => Some(a),
        Command::Migrate(a) => Some(&a.selection),
        _ => match component(&cli.command) {
            Some(ComponentCommand::Serve(a)) => Some(&a.selection),
            Some(ComponentCommand::Config {
                command: LocalConfigCommand::Validate(a),
            }) => Some(a),
            _ => None,
        },
    }
}
fn identifier(value: &str) -> Result<()> {
    Identifier::try_from(value.to_owned()).map(|_| ()).map_err(|_| Error::usage("Local identifiers require 1–64 ASCII letters, digits, dot, underscore, colon or hyphen"))
}
pub fn preflight(cli: &Cli) -> Result<()> {
    if cli.timeout() < Duration::from_millis(1) || cli.timeout() > Duration::from_secs(3600) {
        return Err(Error::usage("Local timeout must be 1 ms through one hour"));
    }
    if cli.credential_stdin || cli.credential_file.is_some() {
        return Err(Error::usage(
            "Remote credential input is not accepted by local commands",
        ));
    }
    if cli.format() == Format::Markdown {
        return Err(Error::usage("Local commands do not support Markdown"));
    }
    if let Some(selected) = selection(cli) {
        for value in [
            &selected.deployment,
            &selected.profile,
            &selected.profile_version,
        ]
        .into_iter()
        .flatten()
        {
            identifier(value)?;
        }
    }
    if cli.wait.is_some() {
        if !matches!(
            &cli.command,
            Command::Deployment {
                command: DeploymentCommand::Inspect {
                    operation: Some(_),
                    ..
                }
            }
        ) {
            return Err(Error::usage(
                "Local --wait requires deployment inspect --operation",
            ));
        }
        if !matches!(cli.wait.as_deref(), Some("accepted" | "terminal" | "ready")) {
            return Err(Error::usage(
                "Local --wait accepts accepted, terminal or ready",
            ));
        }
    }
    match &cli.command {
        Command::Deployment { command } => match command {
            DeploymentCommand::Inspect { id, operation } => {
                identifier(id)?;
                if let Some(op) = operation {
                    identifier(op)?;
                }
            }
            DeploymentCommand::Stop {
                id,
                idempotency_key,
                ..
            } => {
                identifier(id)?;
                if let Some(key) = idempotency_key {
                    identifier(key)?;
                }
            }
            DeploymentCommand::Logs { id, cursor, .. } => {
                identifier(id)?;
                if cli.output.is_some() && cli.output != Some(Format::Json) {
                    return Err(Error::usage("Logs accept text by default or JSON output"));
                }
                if cursor
                    .as_ref()
                    .is_some_and(|v| v.is_empty() || v.len() > 64)
                {
                    return Err(Error::usage("Log cursors must contain 1 through 64 bytes"));
                }
            }
            DeploymentCommand::List { .. } => {}
        },
        Command::Migrate(a) => {
            identifier(&a.target)?;
            if !a.backup_acknowledged {
                return Err(Error::usage("Migration requires --backup-acknowledged"));
            }
            if let Some(key) = &a.idempotency_key {
                identifier(key)?;
            }
        }
        _ => {
            let args = match &cli.command {
                Command::Serve(a)
                | Command::Api {
                    command: ApiCommand::Serve(a),
                } => Some(a),
                _ => match component(&cli.command) {
                    Some(ComponentCommand::Serve(a)) => Some(a),
                    _ => None,
                },
            };
            if let Some(a) = args {
                if let Some(key) = &a.idempotency_key {
                    identifier(key)?;
                }
                let unique: std::collections::BTreeSet<_> = a.add.iter().collect();
                if unique.len() != a.add.len() {
                    return Err(Error::usage("Optional components must be unique"));
                }
                if matches!(cli.command, Command::Oidc { .. })
                    && a.add.iter().any(|name| name == "oidc")
                {
                    return Err(Error::usage("Optional components must be unique"));
                }
                let unavailable = a
                    .add
                    .iter()
                    .map(String::as_str)
                    .find(|name| *name != "oidc")
                    .or(match cli.command {
                        Command::Admin { .. } => Some("admin"),
                        Command::Storefront { .. } => Some("storefront"),
                        _ => None,
                    });
                if let Some(name) = unavailable {
                    return Err(optional_prerequisite(name));
                }
            }
        }
    }
    Ok(())
}
fn optional_prerequisite(component: &str) -> Error {
    Error::unsupported(match component {
        "admin" => {
            "Admin UI serving requires a published image with verified authentication, startup configuration and readiness"
        }
        "storefront" => {
            "Storefront serving requires a verified reference runtime, publication reads and trusted target selection"
        }
        _ => "Selected component has no registered startup mapping",
    })
}

fn socket(cli: &Cli) -> Result<PathBuf> {
    if let Some(path) = &cli.daemon_socket {
        return Ok(path.clone());
    }
    std::env::var_os("XDG_RUNTIME_DIR")
        .map(PathBuf::from)
        .filter(|p| p.is_absolute())
        .map(|p| p.join("gitstore/gitstored.sock"))
        .ok_or_else(|| {
            Error::new(
                4,
                "NOT_FOUND",
                "Select --daemon-socket or set an absolute XDG_RUNTIME_DIR",
            )
        })
}
pub fn dispatch(cli: &Cli) -> Result<()> {
    // Reserve command syntax without enabling execution before the corresponding
    // profile/backend implementation and capability checks exist.
    let required = match &cli.command {
        Command::Deployment {
            command: DeploymentCommand::List { .. } | DeploymentCommand::Inspect { .. },
        } => "deployment.read",
        Command::Deployment {
            command: DeploymentCommand::Stop { .. },
        } => "deployment.stop",
        Command::Deployment {
            command: DeploymentCommand::Logs { .. },
        } => "stream.read",
        Command::Migrate(_) => "migration.start",
        Command::Serve(_)
        | Command::Api {
            command: ApiCommand::Serve(_),
        } => "deployment.start",
        _ => match component(&cli.command) {
            Some(ComponentCommand::Serve(_)) => "deployment.start",
            _ => "deployment.validate",
        },
    };
    let params = if required == "deployment.validate" {
        Some(validation_params(cli)?)
    } else if required == "deployment.start" {
        Some(start_params(cli)?)
    } else if required == "migration.start" {
        Some(migration_params(cli)?)
    } else {
        None
    };
    let mut capabilities = vec![required];
    if required == "deployment.start"
        && params.as_ref().is_some_and(|value| {
            value
                .get("components")
                .and_then(Value::as_array)
                .is_some_and(|components| components.iter().any(|component| component == "oidc"))
        })
    {
        capabilities.push("deployment.oidc");
    }
    if required == "deployment.start" || required == "deployment.stop" {
        capabilities.extend(["deployment.validate", "operation.read", "deployment.read"]);
    }
    if matches!(&cli.command, Command::Migrate(args) if !args.detach) {
        capabilities.push("operation.read");
    }
    if required == "deployment.start" && serve_args(&cli.command).is_some_and(|args| !args.detach) {
        capabilities.push("operation.cancel");
    }
    if matches!(
        &cli.command,
        Command::Deployment {
            command: DeploymentCommand::Inspect {
                operation: Some(_),
                ..
            }
        }
    ) {
        capabilities.push("operation.read");
    }
    // Installed local commands share first-use user-manager setup. An explicit
    // socket (including its environment override) is always connect-only.
    let automatic = cli.daemon_socket.is_none();
    let mut client =
        crate::daemon_start::connect(&socket(cli)?, &capabilities, cli.timeout(), automatic)
            .map_err(|e| local_error(&e))?;
    if let Command::Deployment { command } = &cli.command {
        match command {
            DeploymentCommand::List { limit, cursor } => {
                let mut params = json!({"limit":limit});
                if let Some(value) = cursor {
                    params["cursor"] = json!(value);
                }
                let result = client
                    .call("deployment.list", params, None, cli.timeout())
                    .map_err(|e| local_error(&e))?;
                return output::emit(&result, cli.format());
            }
            DeploymentCommand::Inspect { id, operation } => {
                let deployment = client
                    .call(
                        "deployment.inspect",
                        json!({"deploymentId":id}),
                        None,
                        cli.timeout(),
                    )
                    .map_err(|e| observe_error(&e, operation.as_deref(), id))?;
                if let Some(op) = operation {
                    let mut record = client
                        .call(
                            "operation.inspect",
                            json!({"operationId":op}),
                            None,
                            cli.timeout(),
                        )
                        .map_err(|e| observe_error(&e, Some(op), id))?;
                    if record["deploymentId"] != *id {
                        return Err(Error::new(
                            5,
                            "DEPLOYMENT_CONFLICT",
                            "Selected operation belongs to another deployment",
                        ));
                    }
                    if let Some(until) = &cli.wait {
                        record = client
                            .call(
                                "operation.wait",
                                json!({"operationId":op,"until":until}),
                                None,
                                cli.timeout(),
                            )
                            .map_err(|e| observe_error(&e, Some(op), id))?;
                    }
                    let failed = matches!(record["state"].as_str(), Some("failed" | "cancelled"));
                    // Re-observe after a wait; the first deployment read is not a snapshot.
                    let deployment = if cli.wait.is_some() {
                        client
                            .call(
                                "deployment.inspect",
                                json!({"deploymentId":id}),
                                None,
                                cli.timeout(),
                            )
                            .map_err(|e| observe_error(&e, Some(op), id))?
                    } else {
                        deployment
                    };
                    output::emit(
                        &json!({"deployment":deployment,"operation":record}),
                        cli.format(),
                    )?;
                    return if failed && cli.wait.is_some() {
                        Err(Error::new(
                            8,
                            "OPERATION_FAILED",
                            "Operation reached an unsuccessful terminal state",
                        ))
                    } else {
                        Ok(())
                    };
                }
                return output::emit(&deployment, cli.format());
            }
            DeploymentCommand::Logs {
                id,
                component,
                cursor,
            } => {
                return observe_logs(
                    cli,
                    &mut client,
                    id,
                    component.as_deref(),
                    cursor.as_deref(),
                );
            }
            DeploymentCommand::Stop {
                id,
                idempotency_key,
                detach,
            } => {
                let key = mutation_key(idempotency_key.as_deref())?;
                eprintln!("Deployment {id}; idempotency key {key}");
                let mut accepted = client
                    .call(
                        "deployment.stop",
                        json!({"deploymentId":id}),
                        Some(&key),
                        cli.timeout(),
                    )
                    .map_err(|e| mutation_error(&e, id, &key))?;
                accepted["idempotencyKey"] = json!(key);
                if *detach {
                    return output::emit(&accepted, cli.format());
                }
                let operation = accepted["operationId"].as_str().ok_or_else(|| {
                    Error::new(
                        1,
                        "LOCAL_FAILURE",
                        "Manager omitted accepted operation identity",
                    )
                })?;
                let record = client
                    .call(
                        "operation.wait",
                        json!({"operationId":operation,"until":"terminal"}),
                        None,
                        cli.timeout(),
                    )
                    .map_err(|e| mutation_error(&e, id, &key))?;
                output::emit(&record, cli.format())?;
                return terminal_success(&record, id, &key);
            }
        }
    }
    if required == "deployment.start" {
        return start(cli, &mut client, params.expect("start parameters prepared"));
    }
    if let Command::Migrate(args) = &cli.command {
        let params = params.expect("migration parameters prepared");
        let id = params["deploymentId"]
            .as_str()
            .expect("deployment selected");
        let key = mutation_key(args.idempotency_key.as_deref())?;
        eprintln!("Deployment {id}; idempotency key {key}");
        let mut accepted = client
            .call("migration.start", params.clone(), Some(&key), cli.timeout())
            .map_err(|error| mutation_error(&error, id, &key))?;
        accepted["idempotencyKey"] = json!(key);
        if args.detach {
            return output::emit(&accepted, cli.format());
        }
        let operation = accepted["operationId"].as_str().ok_or_else(|| {
            Error::new(
                1,
                "LOCAL_FAILURE",
                "Manager omitted accepted migration identity",
            )
        })?;
        eprintln!("Migration operation {operation}");
        let record = client
            .call(
                "operation.wait",
                json!({"operationId":operation,"until":"terminal"}),
                None,
                cli.timeout(),
            )
            .map_err(|error| {
                let mut failure = observe_error(&error, Some(operation), id);
                failure
                    .local_identity
                    .get_or_insert_with(Default::default)
                    .idempotency_key = Some(key.clone());
                failure
            })?;
        output::emit(&record, cli.format())?;
        return terminal_success(&record, id, &key);
    }
    if let Some(params) = params {
        let result = client
            .call("deployment.validate", params, None, cli.timeout())
            .map_err(|e| local_error(&e))?;
        output::emit(&result, cli.format())?;
        return if result["valid"] == true {
            Ok(())
        } else {
            Err(Error::new(
                8,
                "LOCAL_VALIDATION_FAILED",
                "Local prerequisites are unmet; inspect the validation plan diagnostics",
            ))
        };
    }
    Err(Error::unsupported(
        "M2 local command implementation pending",
    ))
}
fn local_error(error: &ClientError) -> Error {
    let code = match error.code.as_str() {
        "INVALID_ARGUMENT" => "INVALID_ARGUMENT",
        "MALFORMED_FRAME" => "MALFORMED_FRAME",
        "NEGOTIATION_REQUIRED" => "NEGOTIATION_REQUIRED",
        "NOT_FOUND" => "NOT_FOUND",
        "UNAUTHORIZED_PEER" => "UNAUTHORIZED_PEER",
        "INCOMPATIBLE_VERSION" => "INCOMPATIBLE_VERSION",
        "UNSUPPORTED_CAPABILITY" => "UNSUPPORTED_CAPABILITY",
        "DEADLINE_EXCEEDED" => "DEADLINE_EXCEEDED",
        "LOCAL_TRANSPORT" => "LOCAL_TRANSPORT",
        "CURSOR_EXPIRED" => "CURSOR_EXPIRED",
        "RESOURCE_LIMIT" => "RESOURCE_LIMIT",
        "IDEMPOTENCY_CONFLICT" => "IDEMPOTENCY_CONFLICT",
        "DEPLOYMENT_CONFLICT" => "DEPLOYMENT_CONFLICT",
        "OWNERSHIP_UNCERTAIN" => "OWNERSHIP_UNCERTAIN",
        "MIGRATION_CONFLICT" => "MIGRATION_CONFLICT",
        "UNSUPPORTED_HOST" => "UNSUPPORTED_HOST",
        "UNSAFE_CANCELLATION" => "UNSAFE_CANCELLATION",
        "STREAM_OVERFLOW" => "STREAM_OVERFLOW",
        "PROVISIONING_REQUIRED" => "PROVISIONING_REQUIRED",
        "MIGRATION_PRECONDITION" => "MIGRATION_PRECONDITION",
        "OPERATION_FAILED" => "OPERATION_FAILED",
        "INTERNAL" => "INTERNAL",
        _ => "LOCAL_FAILURE",
    };
    let mut result = Error::new(error.exit, code, &error.message);
    result.retryable = error.retryable;
    result.local_identity = Some(Box::new(crate::error::LocalIdentity {
        operation_id: error.operation_id.clone(),
        deployment_id: error.deployment_id.clone(),
        idempotency_key: error.idempotency_key.clone(),
    }));
    result.stage = match error.stage.as_str() {
        "resolve" => "resolve",
        "authorize" => "authorize",
        "negotiate" => "negotiate",
        "transport" => "transport",
        "parse" => "parse",
        "observe" => "observe",
        "runtime" => "runtime",
        "queue" => "queue",
        "stream" => "stream",
        "recovery" => "recovery",
        "cleanup" => "cleanup",
        "migration" => "migration",
        "provision" => "provision",
        "bootstrap" => "bootstrap",
        _ => "validate",
    };
    result
}

#[derive(Default, Deserialize)]
#[serde(deny_unknown_fields)]
struct LocalDefaults {
    deployment: Option<String>,
    profile: Option<String>,
    profile_version: Option<String>,
    datastore: Option<String>,
}
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct LocalConfig {
    version: u32,
    #[serde(default)]
    selection: LocalDefaults,
}
fn defaults() -> Result<LocalDefaults> {
    use std::{
        fs::{self, OpenOptions},
        io::Read,
        os::unix::fs::{MetadataExt, OpenOptionsExt},
        path::Component,
    };
    let base = std::env::var_os("XDG_CONFIG_HOME")
        .map(PathBuf::from)
        .or_else(|| {
            std::env::var_os("HOME").map(|h| {
                if cfg!(target_os = "macos") {
                    PathBuf::from(h).join("Library/Application Support")
                } else {
                    PathBuf::from(h).join(".config")
                }
            })
        });
    let Some(base) = base else {
        return Ok(LocalDefaults::default());
    };
    if !base.is_absolute() {
        return Err(Error::usage(
            "Local configuration directory must be absolute",
        ));
    }
    let path = base.join("gitstore/local.toml");
    let mut prefix = PathBuf::new();
    for part in path.components() {
        if !matches!(part, Component::RootDir | Component::Normal(_)) {
            return Err(Error::usage("Unsafe local configuration path"));
        }
        prefix.push(part);
        match fs::symlink_metadata(&prefix) {
            Ok(m) if m.file_type().is_symlink() => {
                return Err(Error::usage(
                    "Symlinked local configuration paths are unsafe",
                ));
            }
            Ok(_) => {}
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => {
                return Ok(LocalDefaults::default());
            }
            Err(_) => return Err(Error::usage("Cannot inspect local configuration")),
        }
    }
    let file = OpenOptions::new()
        .read(true)
        .custom_flags(libc::O_NOFOLLOW)
        .open(&path)
        .map_err(|_| Error::usage("Cannot read local configuration"))?;
    let metadata = file
        .metadata()
        .map_err(|_| Error::usage("Cannot inspect local configuration"))?;
    let parent = fs::symlink_metadata(path.parent().expect("parent"))
        .map_err(|_| Error::usage("Cannot inspect local configuration directory"))?;
    let uid = unsafe { libc::geteuid() };
    if !metadata.is_file()
        || metadata.uid() != uid
        || metadata.mode() & 0o777 != 0o600
        || metadata.nlink() != 1
        || !parent.is_dir()
        || parent.uid() != uid
        || parent.mode() & 0o777 != 0o700
    {
        return Err(Error::usage(
            "Local configuration requires an owned 0600 file in an owned 0700 directory",
        ));
    }
    let mut text = String::new();
    file.take(65537)
        .read_to_string(&mut text)
        .map_err(|_| Error::usage("Cannot read local configuration"))?;
    if text.len() > 65536 {
        return Err(Error::usage("Local configuration exceeds 64 KiB"));
    }
    let config: LocalConfig = toml::from_str(&text).map_err(|_| {
        Error::usage("Invalid local configuration; check version, selection and duplicate keys")
    })?;
    if config.version != 1 {
        return Err(Error::usage("Local configuration version must be 1"));
    }
    Ok(config.selection)
}
fn validation_params(cli: &Cli) -> Result<Value> {
    let selected = selection(cli).ok_or_else(|| Error::usage("Local selection missing"))?;
    let fallback = defaults()?;
    let profile = selected
        .profile
        .as_ref()
        .or(fallback.profile.as_ref())
        .map(String::as_str)
        .unwrap_or("core");
    let version = selected
        .profile_version
        .as_ref()
        .or(fallback.profile_version.as_ref())
        .map(String::as_str)
        .unwrap_or(if matches!(cli.command, Command::Oidc { .. }) {
            "2"
        } else {
            "1"
        });
    let datastore = selected
        .datastore
        .as_ref()
        .or(fallback.datastore.as_ref())
        .map(String::as_str)
        .unwrap_or("memdb");
    let deployment = selected
        .deployment
        .as_ref()
        .or(fallback.deployment.as_ref());
    identifier(profile)?;
    identifier(version)?;
    if !matches!(datastore, "memdb" | "scylladb") {
        return Err(Error::usage("Local datastore must be memdb or scylladb"));
    }
    let name = match cli.command {
        Command::Api { .. } => "api",
        Command::Git { .. } => "git",
        Command::Controller { .. } => "controller",
        Command::Oidc { .. } => "oidc",
        Command::Admin { .. } => "admin",
        Command::Storefront { .. } => "storefront",
        _ => return Err(Error::usage("Component validation command required")),
    };
    let mut params = json!({"profileId":profile,"profileVersion":version,"datastore":datastore,"components":[name]});
    if let Some(id) = deployment {
        identifier(id)?;
        params["deploymentId"] = json!(id);
    }
    Ok(params)
}

fn observe_error(error: &ClientError, operation: Option<&str>, deployment: &str) -> Error {
    let mut result = local_error(error);
    let identity = result.local_identity.get_or_insert_with(Default::default);
    if identity.operation_id.is_none() {
        identity.operation_id = operation.map(str::to_owned);
    }
    if identity.deployment_id.is_none() {
        identity.deployment_id = Some(deployment.to_owned());
    }
    result
}

fn serve_args(command: &Command) -> Option<&ServeArgs> {
    match command {
        Command::Serve(a)
        | Command::Api {
            command: ApiCommand::Serve(a),
        } => Some(a),
        _ => match component(command) {
            Some(ComponentCommand::Serve(a)) => Some(a),
            _ => None,
        },
    }
}
fn start_params(cli: &Cli) -> Result<Value> {
    let args = serve_args(&cli.command).ok_or_else(|| Error::usage("Serve selection missing"))?;
    let fallback = defaults()?;
    let selected = &args.selection;
    let oidc =
        matches!(cli.command, Command::Oidc { .. }) || args.add.iter().any(|name| name == "oidc");
    let id = selected
        .deployment
        .as_ref()
        .or(fallback.deployment.as_ref())
        .ok_or_else(|| {
            Error::usage("Select --deployment or configure an explicit local deployment")
        })?;
    let profile = selected
        .profile
        .as_ref()
        .or(fallback.profile.as_ref())
        .map(String::as_str)
        .unwrap_or("core");
    let version = selected
        .profile_version
        .as_ref()
        .or(fallback.profile_version.as_ref())
        .map(String::as_str)
        .unwrap_or(if oidc { "2" } else { "1" });
    let datastore = selected
        .datastore
        .as_ref()
        .or(fallback.datastore.as_ref())
        .map(String::as_str)
        .unwrap_or("memdb");
    for value in [id.as_str(), profile, version] {
        identifier(value)?;
    }
    if !matches!(datastore, "memdb" | "scylladb") {
        return Err(Error::usage("Local datastore must be memdb or scylladb"));
    }
    if oidc && (profile != "core" || version != "2") {
        return Err(Error::usage(
            "OIDC requires the installed core profile version 2",
        ));
    }
    let mut params = json!({"deploymentId":id,"profileId":profile,"profileVersion":version,"datastore":datastore});
    let name = match cli.command {
        Command::Api { .. } => Some("api"),
        Command::Git { .. } => Some("git"),
        Command::Controller { .. } => Some("controller"),
        Command::Oidc { .. } => Some("oidc"),
        _ => None,
    };
    if name.is_some() || !args.add.is_empty() {
        let mut components = name
            .map(|name| vec![name])
            .unwrap_or_else(|| vec!["api", "git", "controller"]);
        components.extend(args.add.iter().map(String::as_str));
        params["components"] = json!(components);
    }
    Ok(params)
}
fn migration_params(cli: &Cli) -> Result<Value> {
    let Command::Migrate(args) = &cli.command else {
        return Err(Error::usage("Migration selection missing"));
    };
    let fallback = defaults()?;
    let id = args
        .selection
        .deployment
        .as_ref()
        .or(fallback.deployment.as_ref())
        .ok_or_else(|| {
            Error::usage("Select --deployment or configure an explicit local deployment")
        })?;
    identifier(id)?;
    // The existing daemon deployment owns profile/datastore identity. Optional
    // local selections cannot silently redirect a migration to another graph.
    for (selected, default, expected) in [
        (&args.selection.profile, &fallback.profile, "core"),
        (
            &args.selection.profile_version,
            &fallback.profile_version,
            "1",
        ),
        (&args.selection.datastore, &fallback.datastore, "scylladb"),
    ] {
        if selected
            .as_ref()
            .or(default.as_ref())
            .is_some_and(|value| value != expected)
        {
            return Err(Error::usage(
                "Migration requires the core/1 Scylla deployment profile",
            ));
        }
    }
    Ok(
        json!({"deploymentId":id,"targetVersion":args.target,"backupAcknowledged":args.backup_acknowledged}),
    )
}
fn mutation_key(explicit: Option<&str>) -> Result<String> {
    use std::io::Read;
    if let Some(key) = explicit {
        identifier(key)?;
        return Ok(key.to_owned());
    }
    let mut entropy = [0u8; 16];
    std::fs::File::open("/dev/urandom")
        .and_then(|mut f| f.read_exact(&mut entropy))
        .map_err(|_| {
            Error::new(
                1,
                "LOCAL_FAILURE",
                "Cannot generate local operation identity",
            )
        })?;
    Ok(format!(
        "gs-{}",
        entropy
            .iter()
            .map(|byte| format!("{byte:02x}"))
            .collect::<String>()
    ))
}
fn mutation_error(error: &ClientError, deployment: &str, key: &str) -> Error {
    let mut failure = observe_error(error, error.operation_id.as_deref(), deployment);
    let identity = failure.local_identity.get_or_insert_with(Default::default);
    identity.idempotency_key = Some(key.to_owned());
    failure
}
fn terminal_success(record: &Value, deployment: &str, key: &str) -> Result<()> {
    if record["state"] == "succeeded" {
        return Ok(());
    }
    let code = match record["error"]["code"].as_str() {
        Some("OWNERSHIP_UNCERTAIN") => "OWNERSHIP_UNCERTAIN",
        Some("UNSAFE_CANCELLATION") => "UNSAFE_CANCELLATION",
        Some("MIGRATION_PRECONDITION") => "MIGRATION_PRECONDITION",
        Some("MIGRATION_CONFLICT") => "MIGRATION_CONFLICT",
        Some("PROVISIONING_REQUIRED") => "PROVISIONING_REQUIRED",
        _ => "OPERATION_FAILED",
    };
    let mut error = Error::new(
        8,
        code,
        "Owned operation did not reach a successful terminal state; inspect retained identity and cleanup status",
    );
    error.local_identity = Some(Box::new(crate::error::LocalIdentity {
        deployment_id: Some(deployment.to_owned()),
        operation_id: record["operationId"].as_str().map(str::to_owned),
        idempotency_key: Some(key.to_owned()),
    }));
    Err(error)
}
fn start(cli: &Cli, client: &mut Client, mut params: Value) -> Result<()> {
    use std::io::{BufRead, IsTerminal, Read, Write};
    let args = serve_args(&cli.command).expect("serve arguments prepared");
    let id = params["deploymentId"]
        .as_str()
        .expect("deployment prepared")
        .to_owned();
    let key = mutation_key(args.idempotency_key.as_deref())?;
    let assessment = client
        .call("deployment.validate", params.clone(), None, cli.timeout())
        .map_err(|e| mutation_error(&e, &id, &key))?;
    for diagnostic in assessment["diagnostics"]
        .as_array()
        .ok_or_else(|| Error::new(1, "LOCAL_FAILURE", "Manager omitted assessment diagnostics"))?
    {
        match diagnostic["code"].as_str() {
            Some(
                "ASSET_VERIFICATION_UNPERFORMED"
                | "NATIVE_VALIDATION_UNPERFORMED"
                | "OWNERSHIP_CHECK_UNPERFORMED",
            ) => {}
            _ => {
                return Err(Error::new(
                    8,
                    "LOCAL_VALIDATION_FAILED",
                    "Local profile or host prerequisite is unavailable; inspect configuration validation",
                ));
            }
        }
    }
    let assets = assessment["requiredAssets"].as_array().ok_or_else(|| {
        Error::new(
            1,
            "LOCAL_FAILURE",
            "Manager omitted the registered asset plan",
        )
    })?;
    let missing = assets.iter().any(|a| a["requiresDownload"] != false);
    let mut consent = args.allow_provision;
    if missing && !consent {
        if cli.non_interactive || !std::io::stdin().is_terminal() {
            return Err(Error::new(
                8,
                "PROVISIONING_REQUIRED",
                "Missing runtime assets require --allow-provision, verified cache candidates or explicit owner offline inputs",
            ));
        }
        for asset in assets {
            eprintln!(
                "{}: {}@{}; platform {}/{}; size unverified; download at most {} bytes, merged at most {} bytes",
                asset["component"].as_str().unwrap_or("component"),
                asset["source"].as_str().unwrap_or("registered origin"),
                asset["digest"].as_str().unwrap_or("registered digest"),
                asset["os"].as_str().unwrap_or("linux"),
                asset["architecture"].as_str().unwrap_or("native"),
                asset["downloadLimitBytes"],
                asset["mergeLimitBytes"]
            );
        }
        eprint!("Provision these assets? [y/N] ");
        std::io::stderr()
            .flush()
            .map_err(|_| Error::new(1, "OUTPUT", "Cannot display provisioning consent"))?;
        let mut answer = Vec::new();
        std::io::stdin()
            .lock()
            .take(65)
            .read_until(b'\n', &mut answer)
            .map_err(|_| Error::usage("Cannot read provisioning consent"))?;
        consent = answer.len() <= 64
            && matches!(
                std::str::from_utf8(&answer).unwrap_or("").trim(),
                "y" | "Y" | "yes" | "YES"
            );
        if !consent {
            return Err(Error::new(
                8,
                "PROVISIONING_REQUIRED",
                "Runtime provisioning was not authorized",
            ));
        }
    }
    let digest = assessment["effectivePlan"]["assetManifestDigest"]
        .as_str()
        .ok_or_else(|| {
            Error::new(
                1,
                "LOCAL_FAILURE",
                "Manager omitted the immutable approved asset plan",
            )
        })?;
    params["assetManifestDigest"] = json!(digest);
    params["allowProvision"] = json!(consent);
    eprintln!("Deployment {id}; idempotency key {key}");
    eprintln!(
        "memdb catalogue state is ephemeral; repository data and existing private material are retained"
    );
    if !args.detach {
        return foreground_start(cli, client, params, &id, &key);
    }
    let mut accepted = client
        .call("deployment.start", params, Some(&key), cli.timeout())
        .map_err(|e| mutation_error(&e, &id, &key))?;
    accepted["idempotencyKey"] = json!(key);
    output::emit(&accepted, cli.format())
}

fn subscribe_logs(
    cli: &Cli,
    client: &mut Client,
    id: &str,
    component: Option<&str>,
    cursor: Option<&str>,
) -> Result<String> {
    let mut params = json!({"deploymentId":id,"kind":"logs"});
    if let Some(component) = component {
        params["component"] = json!(component);
    }
    if let Some(cursor) = cursor {
        params["cursor"] = json!(cursor);
    }
    let result = client
        .observe_interruptible(
            "stream.subscribe",
            params,
            cli.timeout(),
            &FOREGROUND_INTERRUPTED,
        )
        .map_err(|e| local_error(&e))?;
    result["subscriptionId"]
        .as_str()
        .map(str::to_owned)
        .ok_or_else(|| {
            Error::new(
                2,
                "INVALID_ARGUMENT",
                "Manager omitted subscription identity",
            )
        })
}
fn unsubscribe_logs(cli: &Cli, client: &mut Client, subscription: &str) {
    // Dropping this connection is also observation-only if acknowledgment fails.
    let _ = client.call(
        "stream.unsubscribe",
        json!({"subscriptionId":subscription}),
        None,
        cli.timeout().min(Duration::from_secs(2)),
    );
}
fn emit_log(event: &Value, fd: libc::c_int, json_output: bool) -> Result<bool> {
    let text = if json_output {
        serde_json::to_string(event)
            .map_err(|_| Error::new(1, "OUTPUT", "Cannot encode log event"))?
    } else if event["type"] == "gap" {
        format!(
            "Log observation ended: {}",
            event["payload"]["code"].as_str().unwrap_or("INTERNAL")
        )
    } else {
        event["payload"]["text"]
            .as_str()
            .ok_or_else(|| Error::new(2, "INVALID_ARGUMENT", "Invalid log text payload"))?
            .to_owned()
    } + "\n";
    let mut remaining = text.as_bytes();
    while !remaining.is_empty() {
        if FOREGROUND_INTERRUPTED.load(std::sync::atomic::Ordering::Relaxed) {
            return Ok(false);
        }
        let mut poll = libc::pollfd {
            fd,
            events: libc::POLLOUT,
            revents: 0,
        };
        let ready = unsafe { libc::poll(&mut poll, 1, 100) };
        if ready < 0 {
            if std::io::Error::last_os_error().kind() == std::io::ErrorKind::Interrupted {
                continue;
            }
            return Err(Error::new(1, "OUTPUT", "Cannot poll log output"));
        }
        if ready == 0 {
            continue;
        }
        // Small pipe writes after POLLOUT preserve interruption responsiveness
        // without changing the inherited descriptor's blocking mode.
        let count = remaining.len().min(512);
        let written = unsafe { libc::write(fd, remaining.as_ptr().cast(), count) };
        if written < 0 {
            if matches!(
                std::io::Error::last_os_error().kind(),
                std::io::ErrorKind::Interrupted | std::io::ErrorKind::WouldBlock
            ) {
                continue;
            }
            return Err(Error::new(1, "OUTPUT", "Cannot write log output"));
        }
        if written == 0 {
            return Err(Error::new(1, "OUTPUT", "Cannot write log output"));
        }
        remaining = &remaining[written as usize..];
    }
    Ok(true)
}
fn observe_logs(
    cli: &Cli,
    client: &mut Client,
    id: &str,
    component: Option<&str>,
    cursor: Option<&str>,
) -> Result<()> {
    let _signals = ForegroundSignals::install()?;
    let subscription = match subscribe_logs(cli, client, id, component, cursor) {
        Ok(subscription) => subscription,
        Err(_) if FOREGROUND_INTERRUPTED.load(std::sync::atomic::Ordering::Relaxed) => {
            return Ok(());
        }
        Err(error) => return Err(error),
    };
    loop {
        if FOREGROUND_INTERRUPTED.load(std::sync::atomic::Ordering::Relaxed) {
            unsubscribe_logs(cli, client, &subscription);
            return Ok(());
        }
        let Some(event) = client
            .poll_event(Duration::from_millis(100))
            .map_err(|e| local_error(&e))?
        else {
            continue;
        };
        if event["subscriptionId"] != subscription {
            return Err(Error::new(
                2,
                "INVALID_ARGUMENT",
                "Foreign log subscription event",
            ));
        }
        let written = emit_log(
            &event,
            libc::STDOUT_FILENO,
            cli.output == Some(Format::Json),
        )?;
        if !written {
            continue;
        }
        if event["type"] == "gap" {
            let code = match event["payload"]["code"].as_str() {
                Some("CURSOR_EXPIRED") => "CURSOR_EXPIRED",
                Some("STREAM_OVERFLOW") => "STREAM_OVERFLOW",
                _ => "INTERNAL",
            };
            return Err(Error::new(
                8,
                code,
                "Log observation ended; workload state is unchanged",
            ));
        }
    }
}
static FOREGROUND_INTERRUPTED: std::sync::atomic::AtomicBool =
    std::sync::atomic::AtomicBool::new(false);
extern "C" fn foreground_signal(_: libc::c_int) {
    FOREGROUND_INTERRUPTED.store(true, std::sync::atomic::Ordering::Relaxed);
}
struct ForegroundSignals {
    interrupt: libc::sigaction,
    terminate: libc::sigaction,
}
impl ForegroundSignals {
    fn install() -> Result<Self> {
        unsafe {
            FOREGROUND_INTERRUPTED.store(false, std::sync::atomic::Ordering::Relaxed);
            let mut action: libc::sigaction = std::mem::zeroed();
            action.sa_sigaction = foreground_signal as *const () as usize;
            libc::sigemptyset(&mut action.sa_mask);
            let mut interrupt = std::mem::zeroed();
            let mut terminate = std::mem::zeroed();
            if libc::sigaction(libc::SIGINT, &action, &mut interrupt) != 0 {
                return Err(Error::new(
                    1,
                    "LOCAL_FAILURE",
                    "Cannot install foreground interruption handler",
                ));
            }
            if libc::sigaction(libc::SIGTERM, &action, &mut terminate) != 0 {
                libc::sigaction(libc::SIGINT, &interrupt, std::ptr::null_mut());
                return Err(Error::new(
                    1,
                    "LOCAL_FAILURE",
                    "Cannot install foreground interruption handler",
                ));
            }
            Ok(Self {
                interrupt,
                terminate,
            })
        }
    }
}
impl Drop for ForegroundSignals {
    fn drop(&mut self) {
        unsafe {
            libc::sigaction(libc::SIGINT, &self.interrupt, std::ptr::null_mut());
            libc::sigaction(libc::SIGTERM, &self.terminate, std::ptr::null_mut());
        }
    }
}
fn foreground_interrupt(
    cli: &Cli,
    client: &mut Client,
    id: &str,
    operation: &str,
    record: &Value,
) -> Result<()> {
    let key = mutation_key(None)?;
    let terminal = matches!(
        record["state"].as_str(),
        Some("succeeded" | "failed" | "cancelled")
    );
    let (method, params) = if terminal {
        (
            "deployment.stop",
            json!({"deploymentId":id,"expectedOperationId":operation}),
        )
    } else {
        ("operation.cancel", json!({"operationId":operation}))
    };
    eprintln!("Interrupt requested for {operation}; cleanup idempotency key {key}");
    let accepted = client
        .call(method, params, Some(&key), cli.timeout())
        .map_err(|e| mutation_error(&e, id, &key))?;
    let cleanup = accepted["operationId"].as_str().ok_or_else(|| {
        Error::new(
            1,
            "LOCAL_FAILURE",
            "Manager omitted cleanup operation identity",
        )
    })?;
    let observed = client
        .call(
            "operation.wait",
            json!({"operationId":cleanup,"until":"terminal"}),
            None,
            cli.timeout(),
        )
        .map_err(|e| mutation_error(&e, id, &key))?;
    terminal_success(&observed, id, &key)?;
    eprintln!("Owned cleanup observed; retained data preserved");
    Ok(())
}
fn foreground_start(
    cli: &Cli,
    client: &mut Client,
    params: Value,
    id: &str,
    key: &str,
) -> Result<()> {
    let _signals = ForegroundSignals::install()?;
    let accepted = client
        .call("deployment.start", params, Some(key), cli.timeout())
        .map_err(|e| mutation_error(&e, id, key))?;
    let operation = accepted["operationId"].as_str().ok_or_else(|| {
        Error::new(
            1,
            "LOCAL_FAILURE",
            "Manager omitted accepted operation identity",
        )
    })?;
    eprintln!("Operation {operation} accepted; waiting for current-generation readiness");
    let mut log_subscription = if client.supports("stream.read") {
        match subscribe_logs(cli, client, id, None, None) {
            Ok(subscription) => Some(subscription),
            Err(error) => {
                eprintln!("Log observation unavailable: {}", error.message);
                None
            }
        }
    } else {
        None
    };
    let mut emitted = false;
    let mut progress = String::new();
    let mut progress_count = 0;
    loop {
        if let Some(subscription) = log_subscription.clone() {
            for _ in 0..32 {
                match client.poll_event(Duration::from_millis(1)) {
                    Ok(Some(event)) => {
                        if event["subscriptionId"] != subscription {
                            return Err(Error::new(
                                2,
                                "INVALID_ARGUMENT",
                                "Foreign foreground log subscription",
                            ));
                        }
                        let written = emit_log(
                            &event,
                            libc::STDERR_FILENO,
                            cli.output == Some(Format::Json),
                        )?;
                        if !written {
                            break;
                        }
                        if event["type"] == "gap" {
                            log_subscription = None;
                            break;
                        }
                    }
                    Ok(None) => break,
                    Err(error) => return Err(local_error(&error)),
                }
            }
        }
        let record = client
            .call(
                "operation.inspect",
                json!({"operationId":operation}),
                None,
                cli.timeout(),
            )
            .map_err(|e| mutation_error(&e, id, key))?;
        let deployment = client
            .call(
                "deployment.inspect",
                json!({"deploymentId":id}),
                None,
                cli.timeout(),
            )
            .map_err(|e| mutation_error(&e, id, key))?;
        if deployment["lastOperationId"] != operation || deployment["desiredState"] == "stopped" {
            if !emitted {
                output::emit(
                    &json!({"deployment":deployment,"operation":record,"idempotencyKey":key}),
                    cli.format(),
                )?;
            }
            eprintln!(
                "Stopped observing {operation}; its generation is no longer the running target"
            );
            return Ok(());
        }
        if FOREGROUND_INTERRUPTED.load(std::sync::atomic::Ordering::Relaxed) {
            return foreground_interrupt(cli, client, id, operation, &record);
        }
        if matches!(record["state"].as_str(), Some("failed" | "cancelled")) {
            return terminal_success(&record, id, key);
        }
        if !emitted
            && record["state"] == "succeeded"
            && deployment["readiness"] == "ready"
            && deployment["recovery"] == "normal"
            && deployment["generation"] == deployment["observedGeneration"]
        {
            output::emit(
                &json!({"deployment":deployment,"operation":record,"idempotencyKey":key}),
                cli.format(),
            )?;
            emitted = true;
            eprintln!("Deployment {id} ready; interrupt to request owned cleanup");
        }
        let phase = format!("{}:{}", record["state"], deployment["readiness"]);
        if phase != progress && progress_count < 64 {
            eprintln!(
                "Operation {operation}: {} / {}",
                record["state"].as_str().unwrap_or("unknown"),
                deployment["readiness"].as_str().unwrap_or("unknown")
            );
            progress = phase;
            progress_count += 1;
        }
        std::thread::sleep(Duration::from_millis(100));
    }
}
