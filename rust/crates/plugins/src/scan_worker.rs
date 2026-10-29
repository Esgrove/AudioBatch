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
        match receiver.try_recv() {
            Ok(line) => {
                let reply = match serde_json::from_str::<ScanRequest>(&line) {
                    Ok(request) => scan_bundle(&ui, &request.bundle),
                    Err(error) => ScanReply {
                        plugins: Vec::new(),
                        error: Some(format!("Invalid scan request: {error}")),
                    },
                };
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

/// Loads a bundle and describes its audio effect classes.
fn scan_bundle(ui: &UiThread, bundle: &Path) -> ScanReply {
    eprintln!("Scanning {}", bundle.display());
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
