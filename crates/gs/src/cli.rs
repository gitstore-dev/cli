use crate::error::{Error, Result};
use clap::{Args, Parser, Subcommand, ValueEnum};
use std::{path::PathBuf, time::Duration};
#[derive(Parser)]
#[command(
    name = "gs",
    version,
    about = "GitStore remote catalogue CLI",
    disable_help_subcommand = true
)]
pub struct Cli {
    #[arg(long, env = "GITSTORE_CONFIG", global = true)]
    pub config: Option<PathBuf>,
    #[arg(long, env = "GITSTORE_CONTEXT", global = true)]
    pub context: Option<String>,
    #[arg(long, env = "GITSTORE_ENDPOINT", global = true)]
    pub endpoint: Option<String>,
    #[arg(long, env = "GITSTORE_ACCOUNT", global = true)]
    pub account: Option<String>,
    #[arg(long, env = "GITSTORE_NAMESPACE", global = true)]
    pub namespace: Option<String>,
    #[arg(long = "repository", env = "GITSTORE_REPOSITORY", global = true)]
    pub repository: Option<String>,
    #[arg(long = "ref", env = "GITSTORE_REF", global = true)]
    pub git_ref: Option<String>,
    #[arg(long, global = true, conflicts_with = "credential_stdin")]
    pub credential_file: Option<PathBuf>,
    #[arg(long, global = true)]
    pub credential_stdin: bool,
    #[arg(long,env="GITSTORE_NO_INPUT",global=true,default_value="false",value_parser=clap::builder::BoolishValueParser::new())]
    pub non_interactive: bool,
    #[arg(long,global=true,value_parser=parse_duration,default_value="30s")]
    pub timeout: Duration,
    #[arg(short = 'o', long, global = true, value_enum)]
    pub output: Option<Format>,
    #[arg(long, global = true)]
    pub wait: Option<String>,
    #[command(subcommand)]
    pub command: Command,
}
fn parse_duration(s: &str) -> std::result::Result<Duration, String> {
    let d = humantime::parse_duration(s)
        .map_err(|_| "Use a positive duration, such as 30s".to_string())?;
    if d.is_zero() || d.as_millis() > i32::MAX as u128 {
        return Err("Duration must be positive and at most 24 days".into());
    }
    Ok(d)
}
#[derive(Clone, Copy, PartialEq, Eq, ValueEnum)]
pub enum Format {
    Json,
    Yaml,
    Table,
    Markdown,
}
#[derive(Subcommand)]
pub enum Command {
    Context {
        #[command(subcommand)]
        command: ContextCommand,
    },
    Config {
        #[command(subcommand)]
        command: ConfigCommand,
    },
    Auth {
        #[command(subcommand)]
        command: AuthCommand,
    },
    Api {
        #[command(subcommand)]
        command: ApiCommand,
    },
    Read(Selection),
    List(ListArgs),
    Delete(Selection),
    Create(CreateArgs),
    Repository {
        #[command(subcommand)]
        command: RepositoryCommand,
    },
    Update(CreateArgs),
    Watch {
        kind: String,
    },
    ApiResources,
    Explain {
        resource: String,
    },
}
#[derive(Subcommand)]
pub enum ContextCommand {
    List,
    Show,
    Use { name: String },
}
#[derive(Subcommand)]
pub enum ConfigCommand {
    Validate,
}
#[derive(Subcommand)]
pub enum AuthCommand {
    Status,
    Token,
    Logout,
    Switch { account: String },
    Login,
    Refresh,
}
#[derive(Subcommand)]
pub enum ApiCommand {
    Graphql(GraphqlArgs),
}
#[derive(Args)]
pub struct GraphqlArgs {
    #[arg(short='f',action=clap::ArgAction::Append)]
    pub fields: Vec<String>,
    #[arg(short='F',action=clap::ArgAction::Append)]
    pub typed: Vec<String>,
    #[arg(long,conflicts_with_all=["fields","typed"])]
    pub input: Option<String>,
}
#[derive(Args)]
pub struct Selection {
    pub kind: String,
    #[arg(conflicts_with = "id")]
    pub name: Option<String>,
    #[arg(long, conflicts_with = "name")]
    pub id: Option<String>,
}
#[derive(Args)]
pub struct ListArgs {
    pub kind: String,
    #[arg(long,default_value_t=100,value_parser=clap::value_parser!(u16).range(1..=100))]
    pub first: u16,
    #[arg(long)]
    pub after: Option<String>,
}
#[derive(Args)]
pub struct CreateArgs {
    #[arg(short='f',required=true,action=clap::ArgAction::Append)]
    pub files: Vec<String>,
}
#[derive(Subcommand)]
pub enum RepositoryCommand {
    Clone {
        repository: String,
        directory: Option<PathBuf>,
        #[arg(long, default_value = "gitstore")]
        remote: String,
    },
    Sync {
        repository: Option<String>,
        #[arg(long)]
        remote: Option<String>,
    },
}
impl Cli {
    pub fn format(&self) -> Format {
        self.output
            .unwrap_or(if matches!(self.command, Command::Api { .. }) {
                Format::Json
            } else {
                Format::Table
            })
    }
    pub fn preflight(&self) -> Result<()> {
        if self.wait.is_some() {
            return Err(Error::unsupported("DEP-004"));
        }
        for s in [
            &self.context,
            &self.endpoint,
            &self.account,
            &self.namespace,
            &self.repository,
            &self.git_ref,
        ]
        .into_iter()
        .flatten()
        {
            if s.is_empty() {
                return Err(Error::usage("Explicit selectors cannot be empty"));
            }
        }
        match &self.command {
            Command::Auth {
                command: AuthCommand::Switch { account },
            } if self.account.as_ref().is_some_and(|a| a != account) => {
                return Err(Error::usage("Account argument conflicts with --account"));
            }
            Command::Context {
                command: ContextCommand::Use { name },
            } if self.context.as_ref().is_some_and(|c| c != name) => {
                return Err(Error::usage("Context argument conflicts with --context"));
            }
            _ => {}
        }
        let f = self.format();
        match &self.command {
            Command::Auth {
                command: AuthCommand::Login | AuthCommand::Refresh,
            } => return Err(Error::unsupported("DEP-001")),
            Command::Update(_) => return Err(Error::unsupported("DEP-009")),
            Command::Watch { .. } => return Err(Error::unsupported("DEP-017")),
            Command::ApiResources | Command::Explain { .. } => {
                return Err(Error::unsupported("DEP-001/DEP-006"));
            }
            Command::Auth {
                command: AuthCommand::Token,
            } if self.output.is_some() => {
                return Err(Error::usage("auth token accepts raw output only"));
            }
            Command::Auth {
                command: AuthCommand::Logout,
            } if self.credential_file.is_some() || self.credential_stdin => {
                return Err(Error::usage(
                    "Logout removes only the selected OS-store credential; external input is not removed",
                ));
            }
            Command::Api { .. } if f != Format::Json => {
                return Err(Error::usage("GraphQL accepts JSON output only"));
            }
            Command::Read(_) | Command::List(_) | Command::Create(_) | Command::Delete(_)
                if matches!(f, Format::Yaml | Format::Markdown) =>
            {
                return Err(Error::unsupported("DEP-005"));
            }
            _ if f == Format::Markdown => {
                return Err(Error::usage(
                    "Markdown output is not available for this command",
                ));
            }
            _ => {}
        }
        let consumers = usize::from(self.credential_stdin)
            + match &self.command {
                Command::Api {
                    command: ApiCommand::Graphql(a),
                } => {
                    usize::from(a.input.as_deref() == Some("-"))
                        + a.typed
                            .iter()
                            .filter(|s| s.split_once('=').is_some_and(|(_, v)| v == "@-"))
                            .count()
                }
                Command::Create(a) => a.files.iter().filter(|s| s.as_str() == "-").count(),
                _ => 0,
            };
        if consumers > 1 {
            return Err(Error::usage("Stdin may have only one consumer"));
        }
        Ok(())
    }
}
