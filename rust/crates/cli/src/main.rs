//! Entry point of `audiobatch-rs`, the command line front end of the `AudioBatch` Rust port.
//!
//! Translates argument errors and write failures into exit code 1, failed files into exit code 2,
//! and prints help and version with exit code 0, like the C++ CLI.

use std::io::{self, Write};
use std::process::ExitCode;

use audiobatch_cli::args::CliArgs;
use audiobatch_cli::run::{EXIT_USAGE_ERROR, run_analysis};
use audiobatch_core::AnalysisCache;
use clap::Parser;
use clap::error::ErrorKind;
use colored::Colorize;

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
    let cache = match AnalysisCache::open_default() {
        Ok(cache) => cache,
        Err(error) => {
            let _ = writeln!(log, "{}", format!("Error: {error:#}").red());
            return ExitCode::from(EXIT_USAGE_ERROR);
        }
    };

    let mut output = io::stdout().lock();
    match run_analysis(&args, cache, &mut output, &mut log) {
        Ok(code) => ExitCode::from(code),
        Err(error) => {
            // Writing fails when stdout is closed early, for example when piping into head.
            let _ = writeln!(log, "{}", format!("Error: {error:#}").red());
            ExitCode::from(EXIT_USAGE_ERROR)
        }
    }
}
