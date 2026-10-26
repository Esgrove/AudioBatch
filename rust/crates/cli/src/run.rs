//! The analysis workflow of `audiobatch-rs`: analyze the inputs, report failures, and print the results.
//!
//! Follows `cli::runAnalysis` of the C++ CLI without the normalization step.
//! Log messages and failures go to stderr, so stdout only carries the results.

use std::io::Write;
use std::path::PathBuf;
use std::sync::Arc;
use std::time::Instant;

use anyhow::Result;
use audiobatch_core::sorting::sort_records;
use audiobatch_core::{AnalysisCache, AnalysisCoordinator, AnalysisEvent, AnalysisOptions, AnalysisRecord};
use colored::Colorize;
use indicatif::{ProgressBar, ProgressDrawTarget, ProgressStyle};

use crate::args::{CliArgs, OutputFormat};
use crate::output::{format_csv, format_json, format_table};

/// Exit code when every file was analyzed.
pub const EXIT_SUCCESS: u8 = 0;

/// Exit code for invalid arguments or an output that could not be written.
pub const EXIT_USAGE_ERROR: u8 = 1;

/// Exit code when at least one file failed, matching the C++ CLI.
pub const EXIT_FAILURES: u8 = 2;

/// Runs the analysis workflow and returns the process exit code.
///
/// The progress bar is drawn to `progress_target`,
/// which is stderr for the CLI and hidden in tests so the bar does not mix into the test output.
///
/// # Errors
///
/// Returns an error when the worker pool cannot be created or the results cannot be written.
pub fn run_analysis(
    args: &CliArgs,
    cache: Arc<AnalysisCache>,
    output: &mut dyn Write,
    log: &mut dyn Write,
    progress_target: ProgressDrawTarget,
) -> Result<u8> {
    let mut input_paths = args.paths.clone();
    if input_paths.is_empty() {
        let current = std::env::current_dir().unwrap_or_else(|_| PathBuf::from("."));
        writeln!(
            log,
            "No input paths provided, using current directory: \"{}\"",
            current.display()
        )?;
        input_paths.push(current);
    }

    let coordinator = AnalysisCoordinator::new(cache, args.worker_count())?;
    let options = AnalysisOptions {
        input_paths,
        recursive: args.recurse,
        refresh: args.refresh,
    };

    let started = Instant::now();
    let mut records = analyze_with_progress(&coordinator, options, progress_target);
    let failures: Vec<&AnalysisRecord> = records.iter().filter(|record| record.has_error()).collect();
    writeln!(
        log,
        "Analysis complete: {:.2} s, {} files ({} failed)",
        started.elapsed().as_secs_f64(),
        records.len(),
        failures.len()
    )?;
    for record in &failures {
        writeln!(
            log,
            "{}",
            format!(
                "Analysis failed: \"{}\", {}",
                record.path.display(),
                record.error_message
            )
            .red()
        )?;
    }
    let exit_code = if failures.is_empty() {
        EXIT_SUCCESS
    } else {
        EXIT_FAILURES
    };

    sort_records(&mut records, args.sort, !args.reverse);
    let text = match args.output {
        OutputFormat::Table => format_table(&records, false),
        OutputFormat::Csv => format_csv(&records)?,
        OutputFormat::Json => format_json(&records)?,
    };
    output.write_all(text.as_bytes())?;
    output.flush()?;
    Ok(exit_code)
}

/// Runs the analysis and collects every record, showing a progress bar on the given target.
/// The stderr target only draws when stderr is a terminal.
#[expect(
    clippy::literal_string_with_formatting_args,
    reason = "the progress template uses indicatif placeholders, not format arguments"
)]
fn analyze_with_progress(
    coordinator: &AnalysisCoordinator,
    options: AnalysisOptions,
    progress_target: ProgressDrawTarget,
) -> Vec<AnalysisRecord> {
    let progress = ProgressBar::with_draw_target(None, progress_target);
    progress.set_style(
        ProgressStyle::with_template("{spinner} [{elapsed_precise}] {bar:40} {pos}/{len} {wide_msg}")
            .unwrap_or_else(|_| ProgressStyle::default_bar()),
    );

    let (_, receiver) = coordinator.start(options);
    let mut records = Vec::new();
    for event in receiver {
        match event {
            AnalysisEvent::Planned { stale_count, .. } => progress.set_length(stale_count as u64),
            AnalysisEvent::Started { path, .. } => {
                progress.set_message(
                    path.file_name()
                        .map(|name| name.to_string_lossy().into_owned())
                        .unwrap_or_default(),
                );
            }
            AnalysisEvent::Finished { record, .. } => {
                if !record.from_cache {
                    progress.inc(1);
                }
                records.push(*record);
            }
            AnalysisEvent::Completed { .. } => break,
            AnalysisEvent::Progress { .. } => {}
        }
    }
    progress.finish_and_clear();
    records
}

#[cfg(test)]
mod test_run_analysis {
    use super::*;

    use clap::Parser;
    use tempfile::TempDir;

    fn run(arguments: &[&str], directory: &TempDir) -> (u8, String, String) {
        let args = CliArgs::try_parse_from(std::iter::once("audiobatch-rs").chain(arguments.iter().copied()))
            .expect("arguments parse");
        let cache = Arc::new(AnalysisCache::with_path(directory.path().join("analysis.db")));
        let mut output = Vec::new();
        let mut log = Vec::new();
        let code =
            run_analysis(&args, cache, &mut output, &mut log, ProgressDrawTarget::hidden()).expect("run succeeds");
        (
            code,
            String::from_utf8(output).expect("utf-8"),
            String::from_utf8(log).expect("utf-8"),
        )
    }

    fn fixture(name: &str) -> String {
        test_fixtures::fixture(name).to_string_lossy().into_owned()
    }

    #[test]
    fn table_reports_the_tone_level() {
        let directory = TempDir::new().expect("temp dir");
        let (code, output, log) = run(&[&fixture("sine_stereo_44100_16.wav")], &directory);
        assert_eq!(code, EXIT_SUCCESS);
        assert!(output.contains("-6.02"), "{output}");
        assert!(output.contains("sine_stereo_44100_16.wav"), "{output}");
        assert!(log.contains("Analysis complete"), "{log}");
    }

    #[test]
    fn broken_file_exits_with_failures() {
        let directory = TempDir::new().expect("temp dir");
        let (code, output, log) = run(&[&fixture("broken.wav")], &directory);
        assert_eq!(code, EXIT_FAILURES);
        assert!(!output.contains("broken.wav"), "{output}");
        assert!(log.contains("Analysis failed"), "{log}");
    }

    #[test]
    fn json_lists_failures_too() {
        let directory = TempDir::new().expect("temp dir");
        let (code, output, _) = run(
            &["-o", "json", &fixture("broken.wav"), &fixture("sine_mono_44100_16.wav")],
            &directory,
        );
        assert_eq!(code, EXIT_FAILURES);
        let rows: Vec<serde_json::Value> = serde_json::from_str(&output).expect("valid json");
        assert_eq!(rows.len(), 2);
        assert_eq!(rows[0]["status"], "analyzed");
        assert_eq!(rows[1]["status"], "failed");
    }
}
