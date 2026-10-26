//! Entry point of `audiobatch-rs`, the command line front end of the `AudioBatch` Rust port.
//!
//! Translates argument errors and write failures into exit code 1, failed files into exit code 2,
//! and prints help and version with exit code 0, like the C++ CLI.

use std::io::{self, Write};
use std::process::ExitCode;

use audiobatch_cli::args::{CliArgs, CliCommand};
use audiobatch_cli::completion::generate_shell_completion;
use audiobatch_cli::run::{EXIT_USAGE_ERROR, run_analysis};
use audiobatch_core::AnalysisCache;
use audiobatch_core::output_file::trash_disposer;
use clap::error::ErrorKind;
use clap::{CommandFactory, Parser};
use colored::Colorize;
use indicatif::ProgressDrawTarget;

fn main() -> ExitCode {
    let args = match CliArgs::try_parse() {
        Ok(args) => args,
        Err(error) if matches!(error.kind(), ErrorKind::DisplayHelp | ErrorKind::DisplayVersion) => {
            let _ = error.print();
            return ExitCode::SUCCESS;
        }
        Err(error) => {
            let _ = error.print();
            return ExitCode::from(EXIT_USAGE_ERROR);
        }
    };

    let mut log = io::stderr().lock();
    let mut output = io::stdout().lock();

    if let Some(CliCommand::Completion {
        shell,
        install,
        verbose,
    }) = args.command
    {
        let command_name = env!("CARGO_BIN_NAME");
        return match generate_shell_completion(shell, CliArgs::command(), install, verbose, command_name, &mut output) {
            Ok(()) => ExitCode::SUCCESS,
            Err(error) => {
                let _ = writeln!(log, "{}", format!("Error: {error:#}").red());
                ExitCode::from(EXIT_USAGE_ERROR)
            }
        };
    }

    let cache = match AnalysisCache::open_default() {
        Ok(cache) => cache,
        Err(error) => {
            let _ = writeln!(log, "{}", format!("Error: {error:#}").red());
            return ExitCode::from(EXIT_USAGE_ERROR);
        }
    };

    match run_analysis(
        &args,
        cache,
        trash_disposer(),
        &mut output,
        &mut log,
        ProgressDrawTarget::stderr,
    ) {
        Ok(code) => ExitCode::from(code),
        Err(error) => {
            // Writing fails when stdout is closed early, for example when piping into head.
            let _ = writeln!(log, "{}", format!("Error: {error:#}").red());
            ExitCode::from(EXIT_USAGE_ERROR)
        }
    }
}
