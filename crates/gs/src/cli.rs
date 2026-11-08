use crate::error::{Error, Result};
use clap::{Args, Parser, Subcommand, ValueEnum};
use std::{path::PathBuf, time::Duration};
const PRERELEASE_NOTICE: &str = "Early alpha: upstream GitStore schemas continue changing incompatibly. No upgrade compatibility, data preservation or recovery guarantees. Keep independent backups; do not use prereleases for irreplaceable data.";
#[derive(Parser)]
#[command(
    name = "gs",
    version,
    about = "GitStore CLI",
    after_long_help = PRERELEASE_NOTICE,
    disable_help_subcommand = true
)]
pub struct Cli {
    #[arg(long, env = "GITSTORE_CONFIG", global = true)]
    pub config: Option<PathBuf>,
    #[arg(long, env = "GITSTORE_CONTEXT", global = true)]
    pub context: Option<String>,
    #[arg(long, env = "GITSTORE_DAEMON_SOCKET", global = true)]
    pub daemon_socket: Option<PathBuf>,
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
    #[arg(long,global=true,value_parser=parse_duration)]
    pub timeout: Option<Duration>,
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
    /// Generate a shell completion script without accessing configuration.
    Completion {
        #[arg(value_enum)]
        shell: clap_complete::Shell,
    },
    /// Generate the Markdown command reference without accessing configuration.
    Reference,
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
    Read(ReadArgs),
    List(ListArgs),
    Delete(Selection),
    Create(CreateArgs),
    Repository {
        #[command(subcommand)]
        command: RepositoryCommand,
    },
    Update(CreateArgs),
    Watch(WatchArgs),
    Serve(ServeArgs),
    Deployment {
        #[command(subcommand)]
        command: DeploymentCommand,
    },
    Migrate(MigrateArgs),
    Git {
        #[command(subcommand)]
        command: ComponentCommand,
    },
    Controller {
        #[command(subcommand)]
        command: ComponentCommand,
    },
    Oidc {
        #[command(subcommand)]
        command: ComponentCommand,
    },
    Admin {
        #[command(subcommand)]
        command: ComponentCommand,
    },
    Storefront {
        #[command(subcommand)]
        command: ComponentCommand,
    },
    ApiResources {
        #[arg(long, default_value = "gitstore-alpha9-2026-11-05", value_parser = ["gitstore-alpha9-2026-11-05", "m0-2026-10-27"])]
        profile: String,
    },
    Explain {
        resource: String,
        #[arg(long, default_value = "gitstore-alpha9-2026-11-05", value_parser = ["gitstore-alpha9-2026-11-05", "m0-2026-10-27"])]
        profile: String,
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
    Logout {
        #[arg(long)]
        remote: bool,
    },
    Switch {
        account: String,
    },
    Login(LoginArgs),
    Refresh,
}
#[derive(Args)]
pub struct LoginArgs {
    #[arg(long)]
    pub username: String,
    #[arg(
        long,
        required_unless_present = "password_stdin",
        conflicts_with = "password_stdin"
    )]
    pub password_file: Option<PathBuf>,
    #[arg(long, conflicts_with = "password_file")]
    pub password_stdin: bool,
}
#[derive(Args)]
pub struct WatchArgs {
    pub kind: String,
    #[arg(conflicts_with = "id")]
    pub name: Option<String>,
    /// ID watches are unavailable until deletion events identify the incarnation.
    #[arg(long, conflicts_with = "name")]
    pub id: Option<String>,
    /// JSON LabelSelectorInput; matchLabels and matchExpressions only.
    #[arg(long)]
    pub selector: Option<String>,
    /// Resume the exact cursor recorded by the matching local checkpoint.
    #[arg(long)]
    pub resource_version: Option<String>,
    /// Private checkpoint path; defaults to per-selection user state.
    #[arg(long)]
    pub checkpoint_file: Option<PathBuf>,
    #[arg(long, value_parser = clap::value_parser!(u32).range(1..=1_000_000))]
    pub max_events: Option<u32>,
    #[arg(long, default_value_t = 1_048_576, value_parser = clap::value_parser!(u32).range(1024..=16_777_216))]
    pub max_frame_bytes: u32,
}
#[derive(Subcommand)]
pub enum ApiCommand {
    Graphql(GraphqlArgs),
    Serve(ServeArgs),
    Config {
        #[command(subcommand)]
        command: LocalConfigCommand,
    },
}
#[derive(Args)]
pub struct LocalSelection {
    #[arg(long, env = "GITSTORE_DEPLOYMENT")]
    pub deployment: Option<String>,
    #[arg(long, env = "GITSTORE_PROFILE")]
    pub profile: Option<String>,
    #[arg(long, env = "GITSTORE_PROFILE_VERSION")]
    pub profile_version: Option<String>,
    #[arg(long, value_parser = ["memdb", "scylladb"])]
    pub datastore: Option<String>,
}
#[derive(Args)]
pub struct ServeArgs {
    #[command(flatten)]
    pub selection: LocalSelection,
    #[arg(long, value_parser = ["oidc", "admin", "storefront"])]
    pub add: Vec<String>,
    #[arg(long)]
    pub detach: bool,
    #[arg(long)]
    pub allow_provision: bool,
    #[arg(long)]
    pub idempotency_key: Option<String>,
}
#[derive(Subcommand)]
pub enum LocalConfigCommand {
    Validate(LocalSelection),
}
#[derive(Subcommand)]
pub enum ComponentCommand {
    Serve(ServeArgs),
    Config {
        #[command(subcommand)]
        command: LocalConfigCommand,
    },
}
#[derive(Subcommand)]
pub enum DeploymentCommand {
    List {
        #[arg(long, default_value_t = 100, value_parser = clap::value_parser!(u16).range(1..=100))]
        limit: u16,
        #[arg(long)]
        cursor: Option<String>,
    },
    Inspect {
        id: String,
        #[arg(long)]
        operation: Option<String>,
    },
    Stop {
        id: String,
        #[arg(long)]
        idempotency_key: Option<String>,
        #[arg(long)]
        detach: bool,
    },
    Logs {
        id: String,
        #[arg(long, value_parser = ["api", "git", "controller", "oidc", "oidc-smtp", "oidc-hydra", "oidc-kratos", "oidc-ui", "oidc-bridge", "admin", "storefront", "scylladb"])]
        component: Option<String>,
        #[arg(long)]
        cursor: Option<String>,
    },
}
#[derive(Args)]
pub struct MigrateArgs {
    #[command(flatten)]
    pub selection: LocalSelection,
    #[arg(long)]
    pub target: String,
    #[arg(long)]
    pub backup_acknowledged: bool,
    #[arg(long)]
    pub idempotency_key: Option<String>,
    #[arg(long)]
    pub detach: bool,
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
pub struct ReadArgs {
    #[command(flatten)]
    pub selection: Selection,
    /// Export only the exact writable authoring projection.
    #[arg(long)]
    pub authoring: bool,
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
    pub fn timeout(&self) -> Duration {
        self.timeout.unwrap_or(
            if crate::deployment::is_local(&self.command) && self.wait.is_some() {
                Duration::from_secs(60)
            } else {
                Duration::from_secs(30)
            },
        )
    }
    pub fn format(&self) -> Format {
        self.output.unwrap_or(
            if matches!(
                self.command,
                Command::Api {
                    command: ApiCommand::Graphql(_)
                } | Command::Watch(_)
            ) {
                Format::Json
            } else {
                Format::Table
            },
        )
    }
    pub fn preflight(&self) -> Result<()> {
        if crate::deployment::is_local(&self.command) {
            return crate::deployment::preflight(self);
        }
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
        if let Command::Read(args) = &self.command
            && args.authoring
        {
            if f != Format::Markdown {
                return Err(Error::usage("--authoring requires -o markdown"));
            }
            if !matches!(
                args.selection.kind.as_str(),
                "namespace" | "repository" | "product" | "category"
            ) {
                return Err(Error::unsupported(
                    "No writable authoring export for this kind",
                ));
            }
        }
        match &self.command {
            Command::Update(_) => return Err(Error::unsupported("DEP-009")),
            Command::Watch(args) => crate::watch::preflight(self, args)?,
            Command::Auth {
                command: AuthCommand::Token,
            } if self.output.is_some() => {
                return Err(Error::usage("auth token accepts raw output only"));
            }
            Command::Auth {
                command: AuthCommand::Logout { remote: false },
            } if self.credential_file.is_some() || self.credential_stdin => {
                return Err(Error::usage(
                    "Logout removes only the selected OS-store credential; external input is not removed",
                ));
            }
            Command::Api {
                command: ApiCommand::Graphql(_),
            } if f != Format::Json => {
                return Err(Error::usage("GraphQL accepts JSON output only"));
            }
            Command::Read(args) if args.authoring => {}
            Command::Read(_) | Command::List(_) if f == Format::Yaml => {}
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
        if matches!(
            self.command,
            Command::Auth {
                command: AuthCommand::Login(_)
                    | AuthCommand::Refresh
                    | AuthCommand::Logout { remote: true }
            }
        ) && self.credential_stdin
        {
            return Err(Error::usage(
                "Session persistence/removal requires an OS store or explicit protected file, not credential stdin",
            ));
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
                Command::Auth {
                    command: AuthCommand::Login(a),
                } => usize::from(a.password_stdin),
                _ => 0,
            };
        if consumers > 1 {
            return Err(Error::usage("Stdin may have only one consumer"));
        }
        Ok(())
    }
}
