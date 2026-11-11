//! The scan worker: the process that loads plugins for a scan, so a plugin that crashes takes only the worker down.
//!
//! The worker runs on its main thread, which becomes the UI thread,
//! and keeps the platform event loop running while it waits for requests,
//! because copy-protected plugins post licensing work to the main run loop while they load.
//! Each audio effect class is created once to read its main bus channel counts.
//! Diagnostics go to stderr, which the parent sends to the worker log.

use std::ffi::OsString;
use std::io::{BufRead as _, Write as _};
use std::path::Path;
use std::time::Duration;

use crossbeam_channel::TryRecvError;
use vst3_host::{Module, PluginInstance, UiThread, pump_platform_events};

use crate::identity::PluginFormat;
use crate::library::{PluginDescription, bundle_modified_ms, error_chain};
use crate::scan_protocol::{ScanReply, ScanRequest, WORKER_FLAG};

/// How long the worker runs the event loop between checks for a request.
const EVENT_PUMP_INTERVAL: Duration = Duration::from_millis(20);

/// Returns true when the command line arguments, including the program name, start a scan worker.
#[must_use]
pub fn is_scan_worker_invocation(arguments: impl IntoIterator<Item = OsString>) -> bool {
    arguments
        .into_iter()
        .nth(1)
        .is_some_and(|argument| argument == WORKER_FLAG)
}

/// Runs the scan worker until its stdin closes, and returns the process exit code.
///
/// The worker writes its diagnostics with `eprintln!` instead of `tracing`,
/// because it installs no subscriber and the parent redirects its stderr into `plugin-scan-worker.log`.
#[must_use]
pub fn run_scan_worker() -> i32 {
    let ui = match UiThread::register_current() {
        Ok(ui) => ui,
        Err(error) => {
            eprintln!("The plugin scan worker could not start: {}", error_chain(&error));
            return 1;
        }
    };
    let (sender, receiver) = crossbeam_channel::unbounded();
    std::thread::spawn(move || {
        for line in std::io::stdin().lock().lines() {
            let Ok(line) = line else {
                break;
            };
            if sender.send(line).is_err() {
                break;
            }
        }
    });
    loop {
        clap_host::service_main_thread(&ui);
        match receiver.try_recv() {
            Ok(line) => {
                let reply = match serde_json::from_str::<ScanRequest>(&line) {
                    Ok(request) => scan_bundle(&ui, &request.bundle, request.format),
                    Err(error) => ScanReply {
                        plugins: Vec::new(),
                        error: Some(format!("Invalid scan request: {error}")),
                    },
                };
                // Attribute destruction failures to the bundle still awaiting this reply, not the next request.
                clap_host::service_main_thread(&ui);
                let mut stdout = std::io::stdout().lock();
                if writeln!(stdout, "{}", reply.to_line())
                    .and_then(|()| stdout.flush())
                    .is_err()
                {
                    return 1;
                }
            }
            Err(TryRecvError::Empty) => pump_platform_events(&ui, EVENT_PUMP_INTERVAL),
            Err(TryRecvError::Disconnected) => return 0,
        }
    }
}

/// Loads a bundle through its declared API and describes its audio effect classes.
fn scan_bundle(ui: &UiThread, bundle: &Path, format: PluginFormat) -> ScanReply {
    eprintln!("Scanning {format} {}", bundle.display());
    match format {
        PluginFormat::Vst3 => scan_vst3_bundle(ui, bundle),
        PluginFormat::Clap => scan_clap_bundle(ui, bundle),
    }
}

/// Loads a VST3 bundle and describes its audio effect classes.
fn scan_vst3_bundle(ui: &UiThread, bundle: &Path) -> ScanReply {
    let module = match Module::load(ui, bundle) {
        Ok(module) => module,
        Err(error) => {
            eprintln!("Could not load {}: {}", bundle.display(), error_chain(&error));
            return ScanReply {
                plugins: Vec::new(),
                error: Some(error_chain(&error)),
            };
        }
    };
    let modified = bundle_modified_ms(bundle);
    let plugins: Vec<PluginDescription> = module
        .classes()
        .iter()
        .filter(|class| class.is_audio_effect())
        .map(|class| {
            let mut description = PluginDescription::from_class(class, bundle, modified);
            match PluginInstance::create(ui, &module, class.id, None) {
                Ok(instance) => {
                    let layout = instance.current_bus_layout(ui);
                    description.main_input_channels =
                        layout.inputs.first().and_then(|count| u32::try_from(*count).ok());
                    description.main_output_channels =
                        layout.outputs.first().and_then(|count| u32::try_from(*count).ok());
                }
                Err(error) => eprintln!("Could not create {}: {}", class.name, error_chain(&error)),
            }
            description
        })
        .collect();
    eprintln!("Found {} plugins in {}", plugins.len(), bundle.display());
    ScanReply { plugins, error: None }
}

/// Loads a CLAP factory and describes every supported audio effect it exports.
fn scan_clap_bundle(ui: &UiThread, bundle: &Path) -> ScanReply {
    let module = match clap_host::Module::load(ui, bundle) {
        Ok(module) => module,
        Err(error) => {
            eprintln!("Could not load {}: {}", bundle.display(), error_chain(&error));
            return ScanReply {
                plugins: Vec::new(),
                error: Some(error_chain(&error)),
            };
        }
    };
    let modified = bundle_modified_ms(bundle);
    let plugins = module
        .classes(ui)
        .iter()
        .filter(|class| class.is_effect)
        .map(|class| {
            let mut description = PluginDescription::from_clap_class(class, bundle, modified);
            match clap_host::PluginInstance::create(ui, &module, &class.id, None) {
                Ok(instance) => {
                    if let Ok(layout) = instance.current_bus_layout(ui) {
                        description.main_input_channels = layout
                            .main_input
                            .and_then(|index| layout.inputs.get(index))
                            .and_then(|count| u32::try_from(*count).ok());
                        description.main_output_channels = layout
                            .outputs
                            .get(layout.main_output)
                            .and_then(|count| u32::try_from(*count).ok());
                    }
                }
                Err(error) => eprintln!("Could not create {}: {}", class.name, error_chain(&error)),
            }
            description
        })
        .collect::<Vec<_>>();
    eprintln!("Found {} plugins in {}", plugins.len(), bundle.display());
    ScanReply { plugins, error: None }
}

#[cfg(test)]
mod test_worker_invocation {
    use super::*;

    #[test]
    fn only_the_first_argument_starts_a_worker() {
        assert!(is_scan_worker_invocation(
            ["audiobatch-rs", WORKER_FLAG].map(OsString::from)
        ));
        assert!(!is_scan_worker_invocation(
            ["audiobatch-rs", "song.wav", WORKER_FLAG].map(OsString::from)
        ));
        assert!(!is_scan_worker_invocation(["audiobatch-rs"].map(OsString::from)));
    }
}
