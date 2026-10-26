//! Command line arguments of `audiobatch-rs`.
//!
//! The options, their short forms, and their validation follow the analysis options of the C++ `audiobatch` CLI.
//! Normalization and the plugin listing are not part of the Rust port.
//! The `completion` subcommand is an addition that the C++ CLI does not have.

use std::path::PathBuf;
use std::thread;

use audiobatch_core::SortMode;
use clap::{Parser, Subcommand, ValueEnum};
use clap_complete::Shell;

/// Lowest upper limit for the worker count, used on machines with few cores.
const MINIMUM_WORKER_LIMIT: usize = 64;

/// Formats for the analysis results printed to stdout.
#[derive(Debug, Default, Clone, Copy, PartialEq, Eq, ValueEnum)]
pub enum OutputFormat {
    /// Aligned columns for reading in a terminal.
    #[default]
    Table,
    /// One header row and one row per file, for spreadsheets.
    Csv,
    /// An array with one object per file, for scripts.
    Json,
}

/// Subcommands that run instead of the analysis.
#[derive(Subcommand, Debug, Clone, PartialEq, Eq)]
pub enum CliCommand {
    /// Generate shell completion script
    #[command(name = "completion")]
    Completion {
        /// Shell to generate completion for
        #[arg(value_enum)]
        shell: Shell,

        /// Install completion script to the shell's completion directory
        #[arg(short = 'I', long)]
        install: bool,

        /// Print where the completion script was installed
        #[arg(short = 'v', long)]
        verbose: bool,
    },
}

/// Analyze audio files for sample peak, true peak, and integrated loudness (EBU R128).
#[derive(Parser, Debug, Clone)]
#[command(name = "audiobatch-rs", author, version)]
pub struct CliArgs {
    /// Subcommand to run instead of the analysis
    #[command(subcommand)]
    pub command: Option<CliCommand>,

    /// Files and directories to analyze, the current directory when none are given
    #[arg(value_hint = clap::ValueHint::AnyPath)]
    pub paths: Vec<PathBuf>,

    /// Recurse into directories
    #[arg(short = 'r', long = "recurse")]
    pub recurse: bool,

    /// Ignore cached analysis and re-analyze files
    #[arg(short = 'f', long)]
    pub refresh: bool,

    /// Override worker count
    #[arg(short = 'j', long, value_name = "COUNT", value_parser = parse_worker_count)]
    pub jobs: Option<usize>,

    /// Sort by peak, lufs, name, or path
    #[arg(short = 's', long, value_name = "MODE", default_value = "peak", value_parser = parse_sort_mode)]
    pub sort: SortMode,

    /// Sort in descending order
    #[arg(long)]
    pub reverse: bool,

    /// Print results as table, csv, or json
    #[arg(short = 'o', long, value_enum, value_name = "FORMAT", default_value_t = OutputFormat::Table)]
    pub output: OutputFormat,
}

impl CliArgs {
    /// Returns the number of worker threads, the CPU count unless overridden.
    #[must_use]
    pub fn worker_count(&self) -> usize {
        self.jobs.unwrap_or_else(available_cpus)
    }
}

/// Returns the number of CPUs available to the process.
fn available_cpus() -> usize {
    thread::available_parallelism().map_or(1, std::num::NonZeroUsize::get)
}

/// Returns the highest accepted worker count.
/// The pool creates every worker up front, so the count is capped well above any useful value.
fn maximum_worker_count() -> usize {
    MINIMUM_WORKER_LIMIT.max(available_cpus() * 4)
}

/// Parses and validates the worker count option.
fn parse_worker_count(value: &str) -> Result<usize, String> {
    let text = value.trim();
    if text.is_empty() || !text.chars().all(|character| character.is_ascii_digit()) {
        return Err(String::from("Worker count must be a whole number greater than zero"));
    }
    let maximum = maximum_worker_count();
    let count = text
        .parse::<usize>()
        .map_err(|_| format!("Worker count must be at most {maximum}"))?;
    if count == 0 {
        return Err(String::from("Worker count must be greater than zero"));
    }
    if count > maximum {
        return Err(format!("Worker count must be at most {maximum}"));
    }
    Ok(count)
}

/// Parses a sort mode name.
fn parse_sort_mode(value: &str) -> Result<SortMode, String> {
    value.parse()
}

#[cfg(test)]
mod test_parse_arguments {
    use super::*;
    use clap::CommandFactory;

    fn parse(arguments: &[&str]) -> Result<CliArgs, clap::Error> {
        CliArgs::try_parse_from(std::iter::once("audiobatch-rs").chain(arguments.iter().copied()))
    }

    #[test]
    fn defaults_without_options() {
        let args = parse(&[]).expect("parses");
        assert_eq!(args.paths, Vec::<PathBuf>::new());
        assert!(!args.recurse);
        assert!(!args.refresh);
        assert_eq!(args.sort, SortMode::Peak);
        assert_eq!(args.output, OutputFormat::Table);
        assert!(args.worker_count() >= 1);
    }

    #[test]
    fn short_and_long_options_parse() {
        let args = parse(&[
            "-r",
            "--refresh",
            "-j",
            "3",
            "--sort",
            "lufs",
            "--reverse",
            "-o",
            "json",
            "a.wav",
        ])
        .expect("parses");
        assert!(args.recurse);
        assert!(args.refresh);
        assert_eq!(args.jobs, Some(3));
        assert_eq!(args.sort, SortMode::Loudness);
        assert!(args.reverse);
        assert_eq!(args.output, OutputFormat::Json);
        assert_eq!(args.paths, vec![PathBuf::from("a.wav")]);
    }

    #[test]
    fn invalid_worker_counts_are_rejected() {
        assert!(parse(&["-j", "0"]).is_err());
        assert!(parse(&["-j", "abc"]).is_err());
        assert!(parse(&["-j", "99999999999999999999"]).is_err());
        assert!(parse(&["--jobs=-1"]).is_err());
    }

    #[test]
    fn unknown_values_are_rejected() {
        assert!(parse(&["--sort", "size"]).is_err());
        assert!(parse(&["--output", "xml"]).is_err());
        assert!(parse(&["--normalize"]).is_err());
    }

    #[test]
    fn completion_command_parses() {
        let args = parse(&["completion", "zsh", "-I", "--verbose"]).expect("parses");
        assert_eq!(
            args.command,
            Some(CliCommand::Completion {
                shell: Shell::Zsh,
                install: true,
                verbose: true,
            })
        );
        assert!(parse(&["completion", "tcsh"]).is_err());
    }

    #[test]
    fn paths_without_a_command_still_parse() {
        let args = parse(&["a.wav", "b.flac"]).expect("parses");
        assert_eq!(args.command, None);
        assert_eq!(args.paths, vec![PathBuf::from("a.wav"), PathBuf::from("b.flac")]);
    }

    #[test]
    fn clap_command_is_valid() {
        CliArgs::command().debug_assert();
    }
}
