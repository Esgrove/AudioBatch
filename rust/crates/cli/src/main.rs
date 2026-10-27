//! Entry point of `audiobatch-rs`, the command line front end of `AudioBatch`.
//!
//! Translates argument errors and write failures into exit code 1, failed files into exit code 2,
//! and prints help and version with exit code 0.
//! Started with the plugin scan worker flag, it serves a plugin scan instead.

use std::io::{self, IsTerminal as _, Write};
use std::process::ExitCode;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};

use anyhow::Result;
use audiobatch_cli::args::{CliArgs, CliCommand, PluginListing};
use audiobatch_cli::completion::generate_shell_completion;
use audiobatch_cli::plugin_list::{PluginListContext, run_plugin_list};
use audiobatch_cli::run::{EXIT_INTERRUPTED, EXIT_USAGE_ERROR, run_analysis};
use audiobatch_cli::{write_error, write_warning};
use audiobatch_core::AnalysisCache;
use audiobatch_core::log_file::{self, FILE_ONLY_TARGET};
use audiobatch_core::output_file::trash_disposer;
use audiobatch_plugins::{PluginDataPaths, WorkerCommand, is_scan_worker_invocation, run_scan_worker};
use clap::error::ErrorKind;
use clap::{CommandFactory, Parser};
use indicatif::ProgressDrawTarget;
use tracing::level_filters::LevelFilter;

/// Set by the first Ctrl-C, which the analysis and normalization poll to stop cleanly.
static INTERRUPTED: AtomicBool = AtomicBool::new(false);

/// Parses the arguments, then prints shell completions, lists plugins, or analyzes and normalizes the given paths.
/// The exit code tells usage errors and failed files apart.
fn main() -> ExitCode {
    // The scan worker flag is not a user option, so it is checked before clap would reject it.
    if is_scan_worker_invocation(std::env::args_os()) {
        return ExitCode::from(u8::try_from(run_scan_worker()).unwrap_or(EXIT_USAGE_ERROR));
    }

    let args = match CliArgs::try_parse() {
        Ok(args) => args,
        Err(error) if matches!(error.kind(), ErrorKind::DisplayHelp | ErrorKind::DisplayVersion) => {
            let _ = error.print();
            return ExitCode::SUCCESS;
        }
        Err(error) => {
            open_log_file();
            let message = error.render().to_string();
            let first_line = message.lines().next().unwrap_or_default();
            tracing::error!(target: FILE_ONLY_TARGET, "{}", first_line.trim_start_matches("error: "));
            let _ = error.print();
            return ExitCode::from(EXIT_USAGE_ERROR);
        }
    };

    // Like help and version, the completion script leaves no log file.
    if !matches!(args.command, Some(CliCommand::Completion { .. })) {
        open_log_file();
    }

    // stderr is locked for each message only, since the worker threads log to it as well.
    let mut log = io::stderr();
    let mut output = io::stdout().lock();

    match args.command {
        Some(CliCommand::Completion {
            shell,
            install,
            verbose,
        }) => {
            let command_name = env!("CARGO_BIN_NAME");
            return match generate_shell_completion(
                shell,
                CliArgs::command(),
                install,
                verbose,
                command_name,
                &mut output,
            ) {
                Ok(()) => ExitCode::SUCCESS,
                Err(error) => {
                    let _ = write_error!(&mut log, "{error:#}");
                    ExitCode::from(EXIT_USAGE_ERROR)
                }
            };
        }
        Some(CliCommand::List(listing)) => {
            return match list_plugins(listing, &mut output, &mut log) {
                Ok(code) => ExitCode::from(code),
                Err(error) => {
                    let _ = write_error!(&mut log, "{error:#}");
                    ExitCode::from(EXIT_USAGE_ERROR)
                }
            };
        }
        None => {}
    }

    // The analysis still works without the cache, it only has to read every file again on the next run.
    let cache = AnalysisCache::open_default().unwrap_or_else(|error| {
        let _ = write_warning!(
            &mut log,
            "Could not open the analysis cache, results are not cached: {error:#}"
        );
        Arc::new(AnalysisCache::in_memory())
    });

    // The first Ctrl-C lets the run stop at a point where no temporary output is left behind,
    // and a second one ends the process at once.
    let handler = ctrlc::set_handler(|| {
        if INTERRUPTED.swap(true, Ordering::SeqCst) {
            std::process::exit(i32::from(EXIT_INTERRUPTED));
        }
    });
    if let Err(error) = handler {
        let _ = write_warning!(&mut log, "Ctrl-C may leave temporary files behind: {error}");
    }

    match run_analysis(
        &args,
        cache,
        trash_disposer(),
        &mut output,
        &mut log,
        ProgressDrawTarget::stderr,
        &|| INTERRUPTED.load(Ordering::SeqCst),
    ) {
        Ok(code) => ExitCode::from(code),
        Err(error) => {
            // Writing fails when stdout is closed early, for example when piping into head.
            let _ = write_error!(&mut log, "{error:#}");
            ExitCode::from(EXIT_USAGE_ERROR)
        }
    }
}

/// Opens the log file, which also prints the warnings and errors of the libraries to stderr.
/// The CLI's own messages are printed by the message helpers, so they only go to the file.
/// Without a log file the CLI still works, so a failure is only reported.
fn open_log_file() {
    if let Err(error) = log_file::install(env!("CARGO_BIN_NAME"), LevelFilter::WARN) {
        let _ = write_warning!(&mut io::stderr(), "{error:#}");
    }
}

/// Lists the known plugins with the default data folder, rescanning with this executable as the worker.
fn list_plugins(listing: PluginListing, output: &mut dyn Write, log: &mut dyn Write) -> Result<u8> {
    let context = PluginListContext {
        data_paths: PluginDataPaths::default(),
        worker: WorkerCommand::current_executable()?,
        show_progress: io::stderr().is_terminal(),
    };
    run_plugin_list(listing, &context, output, log)
}
