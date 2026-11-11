//! CLI rendering through native chain presets with plugin lifecycle work on the main event-loop thread.
//!
//! Discovery and reporting share the analysis workflow, and rendering shares the GUI's processing coordinator.

use std::collections::HashMap;
use std::io::Write;
use std::path::Path;
use std::sync::Arc;
use std::time::Duration;

use anyhow::Result;
use audiobatch_core::operation::OperationOutcome;
use audiobatch_core::output_file::{FileDisposer, delete_disposer};
use audiobatch_core::{AnalysisCache, AnalysisRecord, AnalysisStatus};
use audiobatch_plugins::presets::load_preset_file_strict;
use audiobatch_plugins::{
    PluginDataPaths, PresetStore, ProcessingChain, ProcessingCoordinator, ProcessingEvent, ProcessingOptions,
};
use crossbeam_channel::RecvTimeoutError;
use indicatif::ProgressDrawTarget;
use vst3_host::{UiDispatcher, UiThread, pump_platform_events, ui_channel};

use crate::args::ProcessArgs;
use crate::run::{EXIT_INTERRUPTED, NormalizedRecords, run_workflow};
use crate::{write_failure, write_warning};

/// Bound on event-loop waits while a render or UI job is pending.
const EVENT_INTERVAL: Duration = Duration::from_millis(20);

/// Processing resources whose plugin instances are bound to the main UI thread.
pub struct ProcessContext<'a> {
    /// Shared lifecycle dispatcher, drained by the main thread until all work has returned.
    pub dispatcher: &'a UiDispatcher,
    /// Native preset instantiated separately for each worker after discovery.
    pub chain: audiobatch_plugins::PluginChain,
    /// Output replacement, disposal, and pre-chain gain policy.
    pub options: ProcessingOptions,
    /// Requested number of rendering workers.
    pub workers: usize,
}

/// Loads a native preset and runs analysis and processing while pumping the platform event loop on the main thread.
///
/// All workers and UI-bound teardown are drained before the main thread's job queue is dropped.
///
/// # Errors
///
/// Returns errors for invalid presets, unavailable plugins, worker setup, and output failures.
pub fn run_process(
    args: &ProcessArgs,
    cache: Arc<AnalysisCache>,
    dispose: FileDisposer,
    output: &mut (dyn Write + Send),
    log: &mut (dyn Write + Send),
    is_interrupted: &(dyn Fn() -> bool + Sync),
) -> Result<u8> {
    let analysis = args.analysis_args();
    let normalization = analysis.normalization().map_err(anyhow::Error::msg)?;
    let preset = if is_preset_path(&args.preset) {
        load_preset_file_strict(Path::new(&args.preset))?
    } else {
        PresetStore::new(PluginDataPaths::default().presets).load_strict(&args.preset)?
    };
    let chain = preset.chain();
    ensure_enabled_plugins(&chain)?;
    let ui = UiThread::register_current()?;
    let (dispatcher, jobs) = ui_channel();
    let workers = analysis.worker_count();
    // Instances are created lazily after discovery, so --jobs cannot multiply plugins for an empty input.
    let result = std::thread::scope(|scope| {
        let dispatcher = &dispatcher;
        let handle = scope.spawn(move || {
            let context = ProcessContext {
                dispatcher,
                chain,
                options: ProcessingOptions {
                    overwrite_existing: args.force,
                    dispose: if args.delete {
                        delete_disposer()
                    } else {
                        Arc::clone(&dispose)
                    },
                    normalize_before_plugins: normalization,
                },
                workers,
            };
            run_workflow(
                &analysis,
                cache,
                dispose,
                output,
                log,
                ProgressDrawTarget::stderr,
                is_interrupted,
                Some(&context),
            )
        });
        while !handle.is_finished() {
            jobs.drain_pending(&ui);
            pump_platform_events(&ui, EVENT_INTERVAL);
        }
        let result = handle.join().map_err(|_| anyhow::anyhow!("Processing worker failed"))?;
        jobs.drain_pending(&ui);
        result
    });
    jobs.drain_pending(&ui);
    if is_interrupted() && result.is_err() {
        return Ok(EXIT_INTERRUPTED);
    }
    result
}

/// Processes successful source records and collects only completed outcomes after interruption.
/// The coordinator owns the common publication and cache update behavior.
///
/// # Errors
///
/// Returns errors when chains cannot be instantiated, the worker pool cannot start, or diagnostics cannot be written.
pub fn process_records(
    records: &[AnalysisRecord],
    cache: Arc<AnalysisCache>,
    context: &ProcessContext<'_>,
    log: &mut dyn Write,
    is_interrupted: &dyn Fn() -> bool,
) -> Result<NormalizedRecords> {
    let analyzed: Vec<_> = records.iter().filter(|record| !record.has_error()).cloned().collect();
    let mut reported = NormalizedRecords {
        records: records.iter().filter(|record| record.has_error()).cloned().collect(),
        failure_count: 0,
    };
    if analyzed.is_empty() || is_interrupted() {
        return Ok(reported);
    }
    let chain = context.chain.clone();
    let dispatcher = context.dispatcher.clone();
    let chain_count = context.workers.min(analyzed.len());
    let chains = context.dispatcher.call(
        move |ui| {
            (0..chain_count)
                .map(|_| ProcessingChain::instantiate(ui, &dispatcher, &chain))
                .collect::<Result<Vec<_>, _>>()
        },
        is_interrupted,
    )??;
    let coordinator = ProcessingCoordinator::new(cache, context.workers)?;
    let receiver = coordinator.start(analyzed, context.options.clone(), chains).receiver;
    let mut results = HashMap::new();
    loop {
        if is_interrupted() {
            coordinator.cancel();
        }
        match receiver.recv_timeout(EVENT_INTERVAL) {
            Ok(ProcessingEvent::Finished { result, .. }) => {
                results.insert(result.source.clone(), *result);
            }
            Err(RecvTimeoutError::Disconnected) => break,
            Ok(
                ProcessingEvent::Completed { .. } | ProcessingEvent::Started { .. } | ProcessingEvent::Progress { .. },
            )
            | Err(RecvTimeoutError::Timeout) => {}
        }
    }
    for record in records.iter().filter(|record| !record.has_error()) {
        let Some(result) = results.remove(&record.path) else {
            continue;
        };
        match result.outcome {
            OperationOutcome::Succeeded { output, notice } => {
                if let Some(notice) = notice {
                    write_warning!(log, "Processed \"{}\", {notice}", output.path.display())?;
                }
                reported.records.push(output);
            }
            OperationOutcome::Failed { error } => {
                write_failure!(log, "Processing failed: \"{}\", {error}", record.path.display())?;
                let mut failed = record.clone();
                failed.status = AnalysisStatus::Failed;
                failed.error_message = error;
                reported.records.push(failed);
                reported.failure_count += 1;
            }
            OperationOutcome::CommittedButUnanalyzed { mut output, error } => {
                write_failure!(log, "Processing failed: \"{}\", {error}", record.path.display())?;
                output.status = AnalysisStatus::Failed;
                output.error_message = error;
                reported.records.push(output);
                reported.failure_count += 1;
            }
        }
    }
    if is_interrupted() {
        write_warning!(
            log,
            "Interrupted. Processing cancelled, committed outputs retained (exit {EXIT_INTERRUPTED})."
        )?;
    }
    Ok(reported)
}

/// Tells whether a `--preset` value names a preset file rather than a preset in the presets folder.
/// An existing file, a value with a path separator, or a `.json` extension is a file,
/// which matches the C++ CLI's rule for its `.xml` presets.
fn is_preset_path(preset: &str) -> bool {
    let path = Path::new(preset);
    path.is_file()
        || preset.contains(['/', '\\'])
        || path
            .extension()
            .is_some_and(|extension| extension.eq_ignore_ascii_case("json"))
}

/// Rejects a chain that would copy the input unchanged, before any plugin is loaded or file is touched.
///
/// # Errors
///
/// Returns an error when no slot of the chain is enabled.
fn ensure_enabled_plugins(chain: &audiobatch_plugins::PluginChain) -> Result<()> {
    anyhow::ensure!(
        chain.slots.iter().any(|slot| slot.enabled),
        "The plugin chain has no enabled plugins"
    );
    Ok(())
}

#[cfg(test)]
mod test_process_preset {
    use super::*;

    use std::path::PathBuf;

    use audiobatch_plugins::{ChainSlot, PluginChain, PluginDescription};
    use clap::Parser;
    use tempfile::TempDir;

    use crate::args::{CliArgs, CliCommand};

    /// Returns an effect description that is never loaded.
    fn description() -> PluginDescription {
        PluginDescription {
            name: "Effect".to_owned(),
            vendor: "Vendor".to_owned(),
            version: "1.0".to_owned(),
            category: "Fx".to_owned(),
            class_id: vst3_host::ClassId([7; 16]),
            sdk_version: "VST 3.8.0".to_owned(),
            bundle: PathBuf::from("/plugins/Effect.vst3"),
            bundle_modified_ms: 0,
            is_instrument: false,
            main_input_channels: Some(2),
            main_output_channels: Some(2),
        }
    }

    /// Parses `process` arguments as the binary does.
    fn process_args(arguments: &[&str]) -> ProcessArgs {
        let args = CliArgs::try_parse_from(["audiobatch-rs", "process"].iter().chain(arguments).copied())
            .expect("arguments parse");
        match args.command {
            Some(CliCommand::Process(process)) => process,
            _ => panic!("process command"),
        }
    }

    #[test]
    fn preset_values_with_a_separator_or_json_extension_are_files() {
        assert!(is_preset_path("presets/Master"));
        assert!(is_preset_path("presets\\Master"));
        assert!(is_preset_path("Master.json"));
        assert!(is_preset_path("Master.JSON"));
        assert!(!is_preset_path("Master"));
        assert!(!is_preset_path("Master.xml"));
    }

    #[test]
    fn a_chain_without_enabled_plugins_is_rejected_before_loading() {
        let mut slot = ChainSlot::new(description());
        assert!(
            ensure_enabled_plugins(&PluginChain {
                slots: vec![slot.clone()]
            })
            .is_ok()
        );
        slot.enabled = false;
        let error = ensure_enabled_plugins(&PluginChain { slots: vec![slot] }).expect_err("all bypassed");
        assert_eq!(error.to_string(), "The plugin chain has no enabled plugins");
        assert!(ensure_enabled_plugins(&PluginChain::default()).is_err());
    }

    #[test]
    fn a_missing_preset_file_fails_before_touching_inputs() {
        let directory = TempDir::new().expect("temp dir");
        let source = directory.path().join("song.wav");
        std::fs::copy(test_fixtures::fixture("sine_stereo_44100_16.wav"), &source).expect("copies fixture");
        let preset = directory.path().join("Missing.json");
        let args = process_args(&[
            "-p",
            preset.to_str().expect("utf-8 path"),
            source.to_str().expect("utf-8 path"),
        ]);
        let cache = Arc::new(AnalysisCache::with_path(directory.path().join("analysis.db")));
        let dispose: FileDisposer = Arc::new(|path: &Path| std::fs::remove_file(path).is_ok());
        let (mut output, mut log) = (Vec::new(), Vec::new());
        assert!(run_process(&args, cache, dispose, &mut output, &mut log, &|| false).is_err());
        assert!(source.exists());
        assert!(!directory.path().join("song.aif").exists());
        assert_eq!(output, [] as [u8; 0]);
    }
}
