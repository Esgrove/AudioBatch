//! Command line arguments of `audiobatch-rs`.
//!
//! The options, their short forms, and their validation follow the C++ `audiobatch` CLI.
//! The normalization options are checked after parsing, so their errors read like the C++ ones.
//! The plugin listing is not part of the Rust port.
//! The `completion` subcommand is an addition that the C++ CLI does not have.

use std::path::PathBuf;
use std::thread;

use audiobatch_core::SortMode;
use audiobatch_core::normalize::{
    MAXIMUM_TARGET_LUFS, MAXIMUM_TARGET_PEAK_DBFS, MINIMUM_TARGET_LUFS, MINIMUM_TARGET_PEAK_DBFS, NormalizationMode,
    NormalizationOptions,
};
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

    /// Normalize analyzed files, then report the re-analyzed output peaks
    #[arg(short = 'n', long)]
    pub normalize: bool,

    /// Let --normalize replace an existing output file
    #[arg(short = 'F', long)]
    pub force: bool,

    /// Normalize to this sample peak, -12 to 0 (default 0)
    #[arg(long, value_name = "DBFS", allow_hyphen_values = true)]
    pub peak: Option<String>,

    /// Normalize to this integrated loudness, -24 to -6, with a -0.3 dBTP ceiling
    #[arg(long, value_name = "LUFS", allow_hyphen_values = true)]
    pub lufs: Option<String>,

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

    /// Returns the normalization options of `--normalize`, or `None` without it.
    ///
    /// Keeps the default 0 dBFS sample peak target when neither `--peak` nor `--lufs` is given.
    ///
    /// # Errors
    ///
    /// Returns the C++ CLI message when `--force`, `--peak`, or `--lufs` is given without `--normalize`,
    /// when both targets are given, or when a target is not a number in its range.
    pub fn normalization(&self) -> Result<Option<NormalizationOptions>, String> {
        if self.force && !self.normalize {
            return Err(String::from("--force can only be used with --normalize"));
        }
        if (self.peak.is_some() || self.lufs.is_some()) && !self.normalize {
            return Err(String::from("--peak and --lufs can only be used with --normalize"));
        }
        if !self.normalize {
            return Ok(None);
        }
        let mut options = NormalizationOptions {
            overwrite_existing: self.force,
            ..NormalizationOptions::default()
        };
        match (&self.peak, &self.lufs) {
            (Some(_), Some(_)) => return Err(String::from("--peak and --lufs cannot be used together")),
            (Some(peak), None) => {
                options.mode = NormalizationMode::Peak;
                options.target_peak_dbfs =
                    parse_target_level("--peak", peak, MINIMUM_TARGET_PEAK_DBFS, MAXIMUM_TARGET_PEAK_DBFS)?;
            }
            (None, Some(lufs)) => {
                options.mode = NormalizationMode::Loudness;
                options.target_lufs = parse_target_level("--lufs", lufs, MINIMUM_TARGET_LUFS, MAXIMUM_TARGET_LUFS)?;
            }
            (None, None) => {}
        }
        Ok(Some(options))
    }
}

/// Parses a target level for `--peak` or `--lufs` within the given range.
///
/// Accepts an optional sign, digits, and at most one decimal point, like the C++ CLI.
fn parse_target_level(option: &str, value: &str, minimum: f64, maximum: f64) -> Result<f64, String> {
    let text = value.trim();
    let digits = text.strip_prefix(['-', '+']).unwrap_or(text);
    let numeric = !digits.is_empty()
        && digits
            .chars()
            .all(|character| character.is_ascii_digit() || character == '.')
        && digits.chars().any(|character| character.is_ascii_digit())
        && digits.matches('.').count() <= 1;
    let target = numeric
        .then(|| text.parse::<f64>().ok())
        .flatten()
        .ok_or_else(|| format!("{option} needs a number, for example {option} {minimum}"))?;
    if target < minimum || target > maximum {
        return Err(format!("{option} must be between {minimum} and {maximum}"));
    }
    Ok(target)
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
    }

    /// Parses the arguments and returns the normalization options or their error.
    fn normalization(arguments: &[&str]) -> Result<Option<NormalizationOptions>, String> {
        parse(arguments).expect("parses").normalization()
    }

    #[test]
    fn normalize_defaults_to_a_zero_peak_target() {
        assert!(normalization(&[]).expect("valid").is_none());
        let options = normalization(&["-n"]).expect("valid").expect("normalizes");
        assert_eq!(options.mode, NormalizationMode::Peak);
        assert!(options.target_peak_dbfs.abs() < f64::EPSILON);
        assert!(!options.overwrite_existing);
    }

    #[test]
    fn normalize_targets_parse() {
        let options = normalization(&["--normalize", "--peak", "-1.5", "-F"])
            .expect("valid")
            .expect("normalizes");
        assert_eq!(options.mode, NormalizationMode::Peak);
        assert!((options.target_peak_dbfs + 1.5).abs() < f64::EPSILON);
        assert!(options.overwrite_existing);

        let options = normalization(&["-n", "--lufs", "-16"])
            .expect("valid")
            .expect("normalizes");
        assert_eq!(options.mode, NormalizationMode::Loudness);
        assert!((options.target_lufs + 16.0).abs() < f64::EPSILON);
    }

    #[test]
    fn normalize_options_need_normalize() {
        assert_eq!(
            normalization(&["--force"]).err().as_deref(),
            Some("--force can only be used with --normalize")
        );
        assert_eq!(
            normalization(&["--peak", "-1"]).err().as_deref(),
            Some("--peak and --lufs can only be used with --normalize")
        );
    }

    #[test]
    fn invalid_normalize_targets_are_rejected() {
        assert_eq!(
            normalization(&["-n", "--peak", "-1", "--lufs", "-14"]).err().as_deref(),
            Some("--peak and --lufs cannot be used together")
        );
        assert_eq!(
            normalization(&["-n", "--peak", "loud"]).err().as_deref(),
            Some("--peak needs a number, for example --peak -12")
        );
        assert_eq!(
            normalization(&["-n", "--peak", "1"]).err().as_deref(),
            Some("--peak must be between -12 and 0")
        );
        assert_eq!(
            normalization(&["-n", "--lufs", "-30"]).err().as_deref(),
            Some("--lufs must be between -24 and -6")
        );
        assert!(normalization(&["-n", "--lufs", "1e1"]).is_err());
        assert!(normalization(&["-n", "--lufs", "-1.2.3"]).is_err());
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
