//! The analysis workflow of `audiobatch-rs`: analyze the inputs, normalize them when asked,
//! report failures, and print the results.
//!
//! Log messages and failures go to stderr, so stdout only carries the results.

use std::collections::HashMap;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::{Duration, Instant};

use anyhow::Result;
use audiobatch_core::discovery::normalize_path;
use audiobatch_core::normalize::{NormalizationOptions, find_batch_output_conflicts};
use audiobatch_core::operation::{OperationOutcome, OperationResult};
use audiobatch_core::output_file::FileDisposer;
use audiobatch_core::sorting::sort_records;
use audiobatch_core::{
    AnalysisCache, AnalysisCoordinator, AnalysisEvent, AnalysisOptions, AnalysisRecord, AnalysisStatus,
    NormalizeCoordinator, NormalizeEvent,
};
use clap::CommandFactory as _;
use crossbeam_channel::RecvTimeoutError;
use indicatif::{ProgressBar, ProgressDrawTarget, ProgressStyle};

use crate::args::{CliArgs, OutputFormat};
use crate::output::{format_csv, format_json, format_table};
use crate::{write_error, write_failure, write_info, write_warning};

/// Exit code when every file was analyzed.
pub const EXIT_SUCCESS: u8 = 0;

/// Exit code for invalid arguments or an output that could not be written.
pub const EXIT_USAGE_ERROR: u8 = 1;

/// Exit code when at least one file failed, the same as the C++ CLI, so `scripts/parity.sh` can compare the two.
pub const EXIT_FAILURES: u8 = 2;

/// Exit code after Ctrl-C, the shell convention of 128 plus the signal number of SIGINT.
pub const EXIT_INTERRUPTED: u8 = 130;

/// How often the analysis wait checks for Ctrl-C.
const INTERRUPT_POLL_INTERVAL: Duration = Duration::from_millis(100);

/// Records to report after normalization, and how many files failed to normalize.
struct NormalizedRecords {
    records: Vec<AnalysisRecord>,
    failure_count: usize,
}

/// Runs the analysis workflow and returns the process exit code.
///
/// Files replaced by normalization go to `dispose`, which moves them to the trash for the CLI.
/// The progress bars are drawn to the targets `progress_target` creates,
/// which are stderr for the CLI and hidden in tests so the bars do not mix into the test output.
/// Once `is_interrupted` returns true, the file being normalized stops before its output is committed,
/// so its temporary output is removed, no further file is normalized, and the run returns `EXIT_INTERRUPTED`.
///
/// # Errors
///
/// Returns an error when the worker pool cannot be created or the results cannot be written.
pub fn run_analysis(
    args: &CliArgs,
    cache: Arc<AnalysisCache>,
    dispose: FileDisposer,
    output: &mut dyn Write,
    log: &mut dyn Write,
    progress_target: fn() -> ProgressDrawTarget,
    is_interrupted: &dyn Fn() -> bool,
) -> Result<u8> {
    let normalization = match args.normalization() {
        Ok(normalization) => normalization.map(|options| NormalizationOptions { dispose, ..options }),
        Err(error) => {
            // The same usage line and hint that clap prints after its own argument errors.
            write_error!(log, "{error}")?;
            writeln!(
                log,
                "\n{}\n\nFor more information, try '--help'.",
                CliArgs::command().render_usage()
            )?;
            return Ok(EXIT_USAGE_ERROR);
        }
    };

    let mut input_paths = args.paths.clone();
    if input_paths.is_empty() {
        let current = std::env::current_dir().unwrap_or_else(|_| PathBuf::from("."));
        write_info!(
            log,
            "No input paths provided, using current directory: \"{}\"",
            current.display()
        )?;
        input_paths.push(current);
    }

    let normalization_cache = Arc::clone(&cache);
    let coordinator = AnalysisCoordinator::new(cache, args.worker_count())?;
    let options = AnalysisOptions {
        input_paths,
        recursive: args.recurse,
        refresh: args.refresh,
    };

    let started = Instant::now();
    let mut records = analyze_with_progress(&coordinator, options, progress_target(), is_interrupted);
    if is_interrupted() {
        return Ok(EXIT_INTERRUPTED);
    }
    let failures: Vec<&AnalysisRecord> = records.iter().filter(|record| record.has_error()).collect();
    write_info!(
        log,
        "Analysis complete: {:.2} s, {} files ({} failed)",
        started.elapsed().as_secs_f64(),
        records.len(),
        failures.len()
    )?;
    for record in &failures {
        write_failure!(
            log,
            "Analysis failed: \"{}\", {}",
            record.path.display(),
            record.error_message
        )?;
    }
    let mut failure_count = failures.len();

    sort_records(&mut records, args.sort, !args.reverse);
    let skipped_inputs = find_skipped_inputs(&args.paths, &records);
    for error in &skipped_inputs {
        write_failure!(log, "{error}")?;
    }
    failure_count += skipped_inputs.len();
    let (mut reported, show_full_paths) = if let Some(options) = &normalization {
        let normalized = normalize_records(
            &records,
            options,
            normalization_cache,
            args.worker_count(),
            log,
            progress_target(),
            is_interrupted,
        )?;
        failure_count += normalized.failure_count;
        (normalized.records, true)
    } else {
        (records, false)
    };
    if is_interrupted() {
        return Ok(EXIT_INTERRUPTED);
    }
    sort_records(&mut reported, args.sort, !args.reverse);

    let text = match args.output {
        OutputFormat::Table => format_table(&reported, show_full_paths),
        OutputFormat::Csv => format_csv(&reported)?,
        OutputFormat::Json => format_json(&reported)?,
    };
    output.write_all(text.as_bytes())?;
    output.flush()?;
    Ok(if failure_count == 0 {
        EXIT_SUCCESS
    } else {
        EXIT_FAILURES
    })
}

/// Normalizes the analyzed records on `worker_count` workers and returns the records to report, in input order.
///
/// The work goes through the same `NormalizeCoordinator` the GUI uses,
/// which fails a file that loses a batch output conflict without touching it,
/// removes a converted source from the cache, and stores the analysis of each output.
/// Files that failed analysis are reported as they are, and were already counted as failures.
/// Once `is_interrupted` returns true, the run is cancelled: files being rendered keep their originals,
/// and only the results of files already committed are reported.
fn normalize_records(
    records: &[AnalysisRecord],
    options: &NormalizationOptions,
    cache: Arc<AnalysisCache>,
    worker_count: usize,
    log: &mut dyn Write,
    progress_target: ProgressDrawTarget,
    is_interrupted: &dyn Fn() -> bool,
) -> Result<NormalizedRecords> {
    let analyzed: Vec<AnalysisRecord> = records.iter().filter(|record| !record.has_error()).cloned().collect();
    let conflicts = find_batch_output_conflicts(&analyzed);
    let progress = ProgressBar::with_draw_target(Some(analyzed.len() as u64), progress_target);
    let coordinator = NormalizeCoordinator::new(cache, worker_count)?;
    let (_, receiver) = coordinator.start(analyzed, options.clone());
    let mut results: HashMap<PathBuf, OperationResult> = HashMap::new();
    loop {
        if is_interrupted() {
            coordinator.cancel();
        }
        match receiver.recv_timeout(INTERRUPT_POLL_INTERVAL) {
            Ok(NormalizeEvent::Started { path, .. }) => {
                progress.set_message(file_name_of(&path));
            }
            Ok(NormalizeEvent::Finished { result, .. }) => {
                progress.inc(1);
                results.insert(result.source.clone(), *result);
            }
            Ok(NormalizeEvent::Completed { .. }) | Err(RecvTimeoutError::Disconnected) => break,
            Ok(NormalizeEvent::Progress { .. }) | Err(RecvTimeoutError::Timeout) => {}
        }
    }
    progress.finish_and_clear();

    let mut normalized = NormalizedRecords {
        records: Vec::with_capacity(records.len()),
        failure_count: 0,
    };
    for record in records {
        if record.has_error() {
            normalized.records.push(record.clone());
            continue;
        }
        let Some(result) = results.remove(&record.path) else {
            continue;
        };
        let label = if conflicts.contains(&record.path) {
            "Normalization output conflict"
        } else {
            "Normalization failed"
        };
        match result.outcome {
            OperationOutcome::Failed { error } => {
                log_error(log, label, record, &error)?;
                normalized.records.push(failed_record(record.clone(), error));
                normalized.failure_count += 1;
            }
            OperationOutcome::CommittedButUnanalyzed { output, error } => {
                log_error(log, label, record, &error)?;
                normalized.records.push(failed_record(output, error));
                normalized.failure_count += 1;
            }
            OperationOutcome::Succeeded { output, notice } => {
                if let Some(notice) = notice {
                    write_warning!(log, "Normalized \"{}\", {notice}", output.path.display())?;
                }
                normalized.records.push(output);
            }
        }
    }
    Ok(normalized)
}

/// Returns the file name of a path for the progress message.
fn file_name_of(path: &Path) -> String {
    path.file_name()
        .map(|name| name.to_string_lossy().into_owned())
        .unwrap_or_default()
}

/// Returns an error for every path named on the command line that produced no result:
/// a path that does not exist, or a file the analysis skips because it is hidden or not a supported format.
///
/// Inside a folder those files are skipped on purpose, but a file named on the command line was asked for,
/// so skipping it silently would report success for work that was never done.
fn find_skipped_inputs(input_paths: &[PathBuf], records: &[AnalysisRecord]) -> Vec<String> {
    let mut errors = Vec::new();
    for input in input_paths {
        let full_path = normalize_path(input);
        if !input.exists() {
            errors.push(format!("Input not found: \"{}\"", full_path.display()));
            continue;
        }
        if input.is_dir() || records.iter().any(|record| record.path == full_path) {
            continue;
        }
        let hidden = input
            .file_name()
            .is_some_and(|name| name.to_string_lossy().starts_with('.'));
        let reason = if hidden {
            "Hidden files are not analyzed"
        } else {
            "Not a supported audio file"
        };
        errors.push(format!("{reason}: \"{}\"", full_path.display()));
    }
    errors
}

/// Marks the record as failed with the error, so the CSV and JSON output list it with its reason.
fn failed_record(mut record: AnalysisRecord, error: String) -> AnalysisRecord {
    record.status = AnalysisStatus::Failed;
    record.error_message = error;
    record
}

/// Writes a red error line about a file to the log.
fn log_error(log: &mut dyn Write, label: &str, record: &AnalysisRecord, error: &str) -> Result<()> {
    write_failure!(log, "{label}: \"{}\", {error}", record.path.display())?;
    Ok(())
}

/// Runs the analysis and collects every record, showing a progress bar on the given target.
/// The stderr target only draws when stderr is a terminal.
/// Once `is_interrupted` returns true, the run is cancelled and the records collected so far are returned.
#[expect(
    clippy::literal_string_with_formatting_args,
    reason = "the progress template uses indicatif placeholders, not format arguments"
)]
fn analyze_with_progress(
    coordinator: &AnalysisCoordinator,
    options: AnalysisOptions,
    progress_target: ProgressDrawTarget,
    is_interrupted: &dyn Fn() -> bool,
) -> Vec<AnalysisRecord> {
    let progress = ProgressBar::with_draw_target(None, progress_target);
    progress.set_style(
        ProgressStyle::with_template("{spinner} [{elapsed_precise}] {bar:40} {pos}/{len} {wide_msg}")
            .unwrap_or_else(|_| ProgressStyle::default_bar()),
    );

    let (_, receiver) = coordinator.start(options);
    let mut records = Vec::new();
    loop {
        if is_interrupted() {
            coordinator.cancel();
            break;
        }
        let event = match receiver.recv_timeout(INTERRUPT_POLL_INTERVAL) {
            Ok(event) => event,
            Err(RecvTimeoutError::Timeout) => continue,
            Err(RecvTimeoutError::Disconnected) => break,
        };
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
        let dispose: FileDisposer = Arc::new(|path: &std::path::Path| std::fs::remove_file(path).is_ok());
        let mut output = Vec::new();
        let mut log = Vec::new();
        let code = run_analysis(
            &args,
            cache,
            dispose,
            &mut output,
            &mut log,
            ProgressDrawTarget::hidden,
            &|| false,
        )
        .expect("run succeeds");
        (
            code,
            String::from_utf8(output).expect("utf-8"),
            String::from_utf8(log).expect("utf-8"),
        )
    }

    fn fixture(name: &str) -> String {
        test_fixtures::fixture(name).to_string_lossy().into_owned()
    }

    /// Copies a fixture into a subfolder of the directory, kept apart from the cache, and returns its path.
    fn copy_fixture(directory: &TempDir, fixture_name: &str, name: &str) -> String {
        let folder = directory.path().join("music");
        std::fs::create_dir_all(&folder).expect("creates folder");
        let path = folder.join(name);
        std::fs::copy(test_fixtures::fixture(fixture_name), &path).expect("copies fixture");
        path.to_string_lossy().into_owned()
    }

    #[test]
    fn an_interrupted_normalization_leaves_the_source_and_no_temporary_files() {
        let directory = TempDir::new().expect("temp dir");
        let tone = copy_fixture(&directory, "sine_stereo_44100_16.wav", "song.wav");
        let args = CliArgs::try_parse_from(["audiobatch-rs", "--normalize", &tone]).expect("arguments parse");
        let cache = Arc::new(AnalysisCache::with_path(directory.path().join("analysis.db")));
        let dispose: FileDisposer = Arc::new(|path: &std::path::Path| std::fs::remove_file(path).is_ok());
        let (mut output, mut log) = (Vec::new(), Vec::new());

        let code = run_analysis(
            &args,
            cache,
            dispose,
            &mut output,
            &mut log,
            ProgressDrawTarget::hidden,
            &|| true,
        )
        .expect("run succeeds");

        assert_eq!(code, EXIT_INTERRUPTED);
        let names: Vec<String> = std::fs::read_dir(directory.path().join("music"))
            .expect("lists")
            .filter_map(|entry| entry.ok().map(|entry| entry.file_name().to_string_lossy().into_owned()))
            .collect();
        assert_eq!(names, vec!["song.wav".to_string()]);
    }

    #[test]
    fn skipped_inputs_fail_the_run() {
        let directory = TempDir::new().expect("temp dir");
        let tone = copy_fixture(&directory, "sine_stereo_44100_16.wav", "song.wav");
        let notes = directory.path().join("music").join("notes.txt");
        let hidden = directory.path().join("music").join(".hidden.wav");
        std::fs::write(&notes, b"text").expect("writes notes");
        std::fs::write(&hidden, b"data").expect("writes hidden file");
        let missing = directory.path().join("music").join("missing.wav");
        let paths = [&notes, &hidden, &missing].map(|path| path.to_string_lossy().into_owned());
        let (code, output, log) = run(&["-o", "json", &tone, &paths[0], &paths[1], &paths[2]], &directory);
        assert_eq!(code, EXIT_FAILURES, "{log}");
        assert!(log.contains("Not a supported audio file"), "{log}");
        assert!(log.contains("Hidden files are not analyzed"), "{log}");
        assert!(
            log.contains(&format!("Input not found: \"{}\"", missing.display())),
            "{log}"
        );
        let rows: Vec<serde_json::Value> = serde_json::from_str(&output).expect("valid json");
        assert_eq!(rows.len(), 1);
    }

    #[test]
    fn folder_input_skips_unsupported_files_quietly() {
        let directory = TempDir::new().expect("temp dir");
        copy_fixture(&directory, "sine_stereo_44100_16.wav", "song.wav");
        std::fs::write(directory.path().join("music/notes.txt"), b"text").expect("writes notes");
        let folder = directory.path().join("music").to_string_lossy().into_owned();
        let (code, _, log) = run(&[&folder], &directory);
        assert_eq!(code, EXIT_SUCCESS, "{log}");
    }

    #[test]
    fn normalize_writes_aiff_and_reports_the_output() {
        let directory = TempDir::new().expect("temp dir");
        let source = copy_fixture(&directory, "sine_stereo_44100_16.wav", "song.wav");
        let (code, output, log) = run(&["-n", "-o", "json", &source], &directory);
        assert_eq!(code, EXIT_SUCCESS, "{log}");
        let rows: Vec<serde_json::Value> = serde_json::from_str(&output).expect("valid json");
        assert_eq!(rows.len(), 1);
        let expected = directory.path().join("music").join("song.aif");
        assert_eq!(rows[0]["path"], expected.to_string_lossy().as_ref());
        assert_eq!(rows[0]["format"], "AIFF file");
        assert!(
            rows[0]["peakDbfs"].as_f64().is_some_and(|peak| peak.abs() < 0.01),
            "{output}"
        );
        assert!(!std::path::Path::new(&source).exists());
    }

    #[test]
    fn normalize_runs_files_on_several_workers() {
        let directory = TempDir::new().expect("temp dir");
        for (fixture_name, name) in [
            ("sine_stereo_44100_16.wav", "first.wav"),
            ("sine_mono_44100_16.wav", "second.wav"),
            ("sine_stereo_44100_16.flac", "third.flac"),
        ] {
            copy_fixture(&directory, fixture_name, name);
        }
        let folder = directory.path().join("music").to_string_lossy().into_owned();
        let (code, output, log) = run(&["-n", "-j", "3", "-o", "json", &folder], &directory);
        assert_eq!(code, EXIT_SUCCESS, "{log}");
        let rows: Vec<serde_json::Value> = serde_json::from_str(&output).expect("valid json");
        let mut names: Vec<&str> = rows.iter().filter_map(|row| row["name"].as_str()).collect();
        names.sort_unstable();
        assert_eq!(names, ["first.aif", "second.aif", "third.aif"]);
        for row in &rows {
            assert!(
                row["peakDbfs"].as_f64().is_some_and(|peak| peak.abs() < 0.01),
                "{output}"
            );
        }
    }

    #[test]
    fn normalize_table_shows_full_paths() {
        let directory = TempDir::new().expect("temp dir");
        let source = copy_fixture(&directory, "sine_stereo_44100_16.wav", "song.wav");
        let (code, output, _) = run(&["-n", "--lufs", "-14", &source], &directory);
        assert_eq!(code, EXIT_SUCCESS);
        let expected = directory.path().join("music").join("song.aif");
        assert!(output.contains(expected.to_string_lossy().as_ref()), "{output}");
        assert!(output.contains("-14.0"), "{output}");
    }

    #[test]
    fn normalize_conflict_fails_the_later_file() {
        let directory = TempDir::new().expect("temp dir");
        copy_fixture(&directory, "sine_stereo_44100_16.wav", "song.wav");
        copy_fixture(&directory, "sine_stereo_44100_16.flac", "song.flac");
        let folder = directory.path().join("music").to_string_lossy().into_owned();
        let (code, output, log) = run(&["-n", "-o", "json", &folder], &directory);
        assert_eq!(code, EXIT_FAILURES);
        assert!(log.contains("Normalization output conflict"), "{log}");
        let rows: Vec<serde_json::Value> = serde_json::from_str(&output).expect("valid json");
        let failed: Vec<&serde_json::Value> = rows.iter().filter(|row| row["status"] == "failed").collect();
        assert_eq!(failed.len(), 1, "{output}");
        assert_eq!(failed[0]["name"], "song.wav");
        assert_eq!(failed[0]["error"], "Another selected file is also written to song.aif");
    }

    #[test]
    fn normalize_existing_output_needs_force() {
        let directory = TempDir::new().expect("temp dir");
        let source = copy_fixture(&directory, "sine_stereo_44100_16.wav", "song.wav");
        std::fs::write(directory.path().join("music").join("song.aif"), b"existing").expect("writes");
        let (code, _, log) = run(&["-n", &source], &directory);
        assert_eq!(code, EXIT_FAILURES);
        assert!(log.contains("already exists"), "{log}");
        let (code, _, log) = run(&["-n", "-F", &source], &directory);
        assert_eq!(code, EXIT_SUCCESS, "{log}");
    }

    #[test]
    fn normalize_options_without_normalize_are_usage_errors() {
        let directory = TempDir::new().expect("temp dir");
        let (code, output, log) = run(&["--force", &fixture("sine_mono_44100_16.wav")], &directory);
        assert_eq!(code, EXIT_USAGE_ERROR);
        assert_eq!(output, "");
        assert!(log.contains("--force can only be used with --normalize"), "{log}");
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
