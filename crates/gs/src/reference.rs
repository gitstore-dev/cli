//! Render installed documentation directly from the command definitions.
use crate::{
    cli::{Cli, Command},
    error::{Error, Result},
};
use clap::{ColorChoice, CommandFactory};
use std::io::{self, Write};

pub fn dispatch(cli: &Cli) -> Result<()> {
    if cli.output.is_some()
        || cli.wait.is_some()
        || cli.credential_file.is_some()
        || cli.credential_stdin
    {
        return Err(Error::usage(
            "Reference generation does not accept output, wait or credential input",
        ));
    }
    let mut command = Cli::command().color(ColorChoice::Never);
    command.build();
    let mut output = Vec::new();
    match cli.command {
        Command::Completion { shell } => {
            clap_complete::generate(shell, &mut command, "gs", &mut output)
        }
        Command::Reference => render(&mut command, "gs", &mut output)?,
        _ => return Err(Error::usage("Select completion or reference")),
    }
    io::stdout()
        .lock()
        .write_all(&output)
        .map_err(|_| Error::new(1, "OUTPUT_FAILED", "Cannot write command reference"))
}

fn render(command: &mut clap::Command, name: &str, output: &mut Vec<u8>) -> Result<()> {
    let header = format!("## {name}\n\n```text\n");
    output.extend_from_slice(header.as_bytes());
    command
        .write_long_help(&mut *output)
        .map_err(|_| Error::new(1, "OUTPUT_FAILED", "Cannot render command reference"))?;
    output.extend_from_slice(b"\n```\n\n");
    for child in command.get_subcommands_mut() {
        if !child.is_hide_set() {
            let child_name = format!("{name} {}", child.get_name());
            render(child, &child_name, output)?;
        }
    }
    Ok(())
}
