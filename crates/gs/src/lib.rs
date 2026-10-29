//! GitStore remote CLI. Resource writes use only public GraphQL.
pub mod authoring;
pub mod cli;
pub mod config;
pub mod credentials;
pub mod error;
pub mod git;
pub mod graphql;
pub mod input;
pub mod output;
pub mod resources;
use clap::Parser;
use cli::*;
use credentials::Store;
use error::{Error, Result};
use serde_json::json;
pub fn run() -> i32 {
    let cli = match Cli::try_parse() {
        Ok(c) => c,
        Err(e) => {
            let exit = if e.use_stderr() { 2 } else { 0 };
            if exit == 0 {
                let _ = e.print();
            } else {
                let args: Vec<_> = std::env::args().collect();
                let machine = args.windows(2).any(|a| {
                    matches!(a[0].as_str(), "-o" | "--output")
                        && matches!(a[1].as_str(), "json" | "yaml")
                }) || args
                    .iter()
                    .any(|a| matches!(a.as_str(), "--output=json" | "--output=yaml"));
                output::diagnostic(
                    &Error::usage("Invalid command-line arguments; run gs --help"),
                    machine,
                );
            }
            return exit;
        }
    };
    let result = cli.preflight().and_then(|_| dispatch(&cli));
    match result {
        Ok(()) => 0,
        Err(e) => {
            output::diagnostic(&e, matches!(cli.format(), Format::Json | Format::Yaml));
            e.exit
        }
    }
}
fn dispatch(cli: &Cli) -> Result<()> {
    let prepared = if let Command::Api {
        command: ApiCommand::Graphql(a),
    } = &cli.command
    {
        Some(graphql::prepare(a)?)
    } else {
        None
    };
    match &cli.command {
        Command::Read(s) | Command::Delete(s) => {
            let k = resources::Kind::parse(&s.kind)?;
            if matches!(cli.command, Command::Delete(_)) {
                k.can_delete()?;
            }
            if s.name.is_none() && s.id.is_none() {
                return Err(Error::usage("Select a name or --id"));
            }
        }
        Command::List(a) => {
            resources::list_request(
                resources::Kind::parse(&a.kind)?,
                Some("preflight"),
                a.first,
                a.after.as_deref(),
            )?;
        }
        _ => {}
    }
    let p = config::path(cli)?;
    match &cli.command {
        Command::Context {
            command: ContextCommand::Use { name },
        } => {
            config::mutate(&p, |c| {
                if !c.contexts.contains_key(name) {
                    return Err(Error::usage("Context does not exist"));
                }
                c.current_context = Some(name.clone());
                Ok(())
            })?;
            return output::emit(&json!({"context":name}), cli.format());
        }
        Command::Context {
            command: ContextCommand::List,
        } => {
            let c = config::load(&p)?;
            return output::emit(&json!(c.contexts.iter().map(|(n,cx)|json!({"name":n,"endpoint":cx.endpoint,"account":cx.account,"selected":Some(n)==c.current_context.as_ref()})).collect::<Vec<_>>()),cli.format());
        }
        _ => {}
    }
    let cfg = config::load(&p)?;
    let e = cfg.effective(cli)?;
    let value = match &cli.command {
        Command::Context {
            command: ContextCommand::Show,
        } => e.summary(),
        Command::Config {
            command: ConfigCommand::Validate,
        } => json!({"valid":true,"effective":e.summary(),"credentialVerification":"not-checked"}),
        Command::Auth {
            command: AuthCommand::Switch { account },
        } => {
            config::mutate(&p, |c| {
                let cx = c
                    .contexts
                    .get_mut(&e.name)
                    .ok_or_else(|| Error::usage("Context missing"))?;
                if !cx.accounts.contains_key(account) {
                    return Err(Error::usage(
                        "Account does not belong to selected installation",
                    ));
                }
                cx.account = account.clone();
                Ok(())
            })?;
            json!({"account":account,"context":e.name})
        }
        Command::Auth {
            command: AuthCommand::Status,
        } => {
            let present = if cli.credential_file.is_some() || cli.credential_stdin {
                credentials::token(cli, &e)?;
                true
            } else {
                credentials::Native
                    .read(&credentials::key(&e, "graphql"))?
                    .is_some()
            };
            json!({"present":present,"verified":false,"account":e.context.account,"endpoint":e.context.endpoint})
        }
        Command::Auth {
            command: AuthCommand::Logout,
        } => {
            json!({"localRemoved":credentials::Native.remove(&credentials::key(&e,"graphql"))?,"remoteInvalidation":"unsupported"})
        }
        Command::Auth {
            command: AuthCommand::Token,
        } => {
            use std::io::Write;
            let token = credentials::token(cli, &e)?;
            writeln!(std::io::stdout().lock(), "{}", token.expose())
                .map_err(|_| Error::new(1, "OUTPUT", "Cannot write token"))?;
            return Ok(());
        }
        Command::Api { .. } => {
            let request = prepared.as_ref().expect("prepared GraphQL request");
            let client = graphql::Client::new(
                &e.context.endpoint,
                credentials::token(cli, &e)?,
                cli.timeout,
            )?;
            let reply = client.execute(request)?;
            output::emit(&reply.body, cli.format())?;
            return reply.error.map_or(Ok(()), Err);
        }
        Command::Read(s) => {
            e.profile()?;
            let r = resources::selection(s, e.context.namespace.as_deref())?;
            let _ = r;
            let client = graphql::Client::new(
                &e.context.endpoint,
                credentials::token(cli, &e)?,
                cli.timeout,
            )?;
            resources::read(&client, s, e.context.namespace.as_deref(), cli.format())?
        }
        Command::List(a) => {
            e.profile()?;
            let kind = resources::Kind::parse(&a.kind)?;
            let _ = resources::list_request(
                kind,
                e.context.namespace.as_deref(),
                a.first,
                a.after.as_deref(),
            )?;
            let client = graphql::Client::new(
                &e.context.endpoint,
                credentials::token(cli, &e)?,
                cli.timeout,
            )?;
            resources::list(
                &client,
                kind,
                e.context.namespace.as_deref(),
                a.first,
                a.after.as_deref(),
                cli.format(),
            )?
        }
        Command::Delete(s) => {
            e.profile()?;
            let _ = resources::selection(s, e.context.namespace.as_deref())?;
            let client = graphql::Client::new(
                &e.context.endpoint,
                credentials::token(cli, &e)?,
                cli.timeout,
            )?;
            resources::delete(&client, s, e.context.namespace.as_deref(), cli.format())?
        }
        Command::Create(a) => {
            e.profile()?;
            let docs = authoring::load(
                &a.files,
                e.context.namespace.as_deref(),
                cli.namespace.is_some(),
            )?;
            let client = graphql::Client::new(
                &e.context.endpoint,
                credentials::token(cli, &e)?,
                cli.timeout,
            )?;
            let (v, error) = authoring::submit(&client, &docs)?;
            output::emit(&v, cli.format())?;
            return error.map_or(Ok(()), Err);
        }
        Command::Repository { command } => git::dispatch(cli, &e, command)?,
        _ => return Err(Error::unsupported("M1 remote implementation pending")),
    };
    output::emit(&value, cli.format())
}
