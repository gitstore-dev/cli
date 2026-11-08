//! Explicit alpha.9 static-user sessions; no provider discovery or blind retries.
use crate::{
    auth_storage::Storage,
    cli::{AuthCommand, Cli, Command},
    config::Effective,
    credentials::{self, Secret},
    error::{Error, Result},
    graphql, input,
};
use serde_json::{Value, json};

fn persistence(message: &str) -> Error {
    let mut error = Error::new(8, "AUTH_PERSISTENCE_UNCERTAIN", message).uncertain();
    error.stage = "persistence";
    error
}
fn token_payload(value: &Value) -> Result<(Secret, i64)> {
    let access = value["accessToken"]
        .as_str()
        .filter(|value| !value.is_empty() && value.len() <= 65536);
    let expires = value["expiresIn"]
        .as_i64()
        .filter(|value| *value > 0 && *value <= i64::from(i32::MAX));
    if value["tokenType"] != "Bearer"
        || access.is_none()
        || value["refreshToken"].as_str() != access
        || !value["scope"].is_null()
        || !value["idToken"].is_null()
        || expires.is_none()
    {
        return Err(Error::remote("Session payload differs from the configured static-user contract; log in again after reviewing configuration").uncertain());
    }
    let secret = Secret::new(access.unwrap_or_default().to_owned())
        .map_err(|_| Error::remote("Invalid issued session material").uncertain())?;
    Ok((secret, expires.unwrap_or_default()))
}
fn existing_session(storage: &Storage) -> Result<Secret> {
    let token = storage.read()?;
    if token.expose().len() > 65536 {
        return Err(Error::auth("Session input exceeds 64 KiB"));
    }
    Ok(token)
}

pub fn dispatch(cli: &Cli, effective: &Effective) -> Result<Value> {
    effective.profile()?;
    let configured = effective.context.accounts[&effective.context.account]
        .auth
        .as_ref()
        .ok_or_else(|| {
            Error::unsupported("DEP-001: configure an explicit versioned static-user provider")
        })?;
    if configured.version != 1
        || configured.provider != "static-users"
        || effective.context.contract_profile.as_deref() != Some("gitstore-alpha9-2026-11-05")
    {
        return Err(Error::unsupported(
            "Only the explicit alpha.9 static-users v1 auth contract is available",
        ));
    }
    let command = match &cli.command {
        Command::Auth { command } => command,
        _ => return Err(Error::usage("Select an explicit authentication operation")),
    };
    if let AuthCommand::Login(args) = command
        && args.username != configured.username
    {
        return Err(Error::usage(
            "Login username must match the configured selected account identity",
        ));
    }
    let storage = Storage::prepare(
        cli.credential_file.as_deref(),
        credentials::key(effective, "graphql"),
    )?;
    let mut previous = None;
    let (field, request, client) = match command {
        AuthCommand::Login(args) => {
            let password = input::password(args.password_file.as_deref(), args.password_stdin)?;
            let request = graphql::validate(
                json!({"query":"mutation Login($input:LoginInput!){login(input:$input){token{accessToken refreshToken tokenType expiresIn scope idToken}}}",
                "variables":{"input":{"username":args.username,"password":password.expose()}}}),
            )?;
            (
                "login",
                request,
                graphql::Client::anonymous(&effective.context.endpoint, cli.timeout())?,
            )
        }
        AuthCommand::Refresh => {
            let token = existing_session(&storage)?;
            let request = graphql::validate(
                json!({"query":"mutation Refresh($input:RefreshTokenInput!){refreshToken(input:$input){token{accessToken refreshToken tokenType expiresIn scope idToken}}}",
                "variables":{"input":{"refreshToken":token.expose()}}}),
            )?;
            previous = Some(token);
            (
                "refreshToken",
                request,
                graphql::Client::anonymous(&effective.context.endpoint, cli.timeout())?,
            )
        }
        AuthCommand::Logout { remote: true } => {
            let token = existing_session(&storage)?;
            let request = graphql::validate(
                json!({"query":"mutation Logout{logout{success}}","variables":{}}),
            )?;
            (
                "logout",
                request,
                graphql::Client::new(&effective.context.endpoint, token, cli.timeout())?,
            )
        }
        _ => return Err(Error::usage("Select login, refresh or logout --remote")),
    };
    let reply = client.execute(&request)?;
    if reply.error.is_some() {
        let mut error=Error::auth("Authentication operation rejected or unavailable; review the explicit provider configuration").uncertain();
        error.stage = "auth";
        return Err(error);
    }
    let payload = &reply.body["data"][field];
    if field == "logout" {
        if payload["success"] != true {
            return Err(Error::auth(
                "Remote invalidation was not confirmed; local credential retained",
            ));
        }
        let removed = storage.remove().map_err(|_| {
            persistence("Remote invalidation succeeded; local credential removal is uncertain")
        })?;
        return Ok(
            json!({"remoteInvalidation":"confirmed","localRemoved":removed,"account":effective.context.account}),
        );
    }
    let (token, expires) = token_payload(&payload["token"])?;
    if previous
        .as_ref()
        .is_some_and(|old| old.expose() == token.expose())
    {
        return Err(Error::remote("Refresh did not return a new session; rotation is uncertain and must not be retried blindly").uncertain());
    }
    storage.write(&token).map_err(|_|persistence("Remote session issued but local persistence is uncertain; log in again rather than retrying refresh"))?;
    Ok(
        json!({"authenticated":true,"persisted":true,"account":effective.context.account,
        "provider":"static-users","expiresIn":expires,"rotation":if field=="refreshToken"{"completed"}else{"not-requested"}}),
    )
}
