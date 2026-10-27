//! Shared helpers for the unit tests of plugin processing.
//!
//! Only one thread per process can be the UI thread, and test threads come and go,
//! so the tests share one dedicated UI thread that runs for the rest of the test process.
//! Jobs on it must not panic, because a panic would end the thread for every later test,
//! so they return their results and the tests assert on the test thread.

use std::fs;
use std::path::Path;
use std::sync::{Arc, OnceLock};

use audiobatch_core::analysis::analyze_file;
use audiobatch_core::decode::AudioFileDecoder;
use audiobatch_core::record::AnalysisRecord;
use audiobatch_test_plugin::Kind;
use tempfile::TempDir;
use vst3_host::{ClassId, Module, PluginInstance, PluginState, UiDispatcher, UiThread};

use crate::processing::ProcessingOptions;
use crate::processing_chain::{ChainPlugin, ProcessingChain};

/// The dispatcher of the shared test UI thread.
static DISPATCHER: OnceLock<UiDispatcher> = OnceLock::new();

/// A decoded file: its channel count, sample rate, and samples per channel.
pub struct DecodedAudio {
    /// Number of channels.
    pub channels: u32,
    /// Sample rate in hertz.
    pub sample_rate: u32,
    /// The samples of each channel.
    pub samples: Vec<Vec<f32>>,
}

impl DecodedAudio {
    /// Returns the number of frames.
    pub fn frames(&self) -> usize {
        self.samples.first().map_or(0, Vec::len)
    }

    /// Returns the largest absolute sample.
    pub fn peak(&self) -> f32 {
        self.samples
            .iter()
            .flatten()
            .fold(0.0_f32, |peak, sample| peak.max(sample.abs()))
    }
}

/// Returns the dispatcher of the shared test UI thread, starting the thread on first use.
pub fn dispatcher() -> &'static UiDispatcher {
    DISPATCHER.get_or_init(|| UiThread::spawn_dedicated().expect("no other UI thread in the test process"))
}

/// Returns the state of the fake gain plugins for a linear gain.
pub fn gain_state(gain: f64) -> PluginState {
    PluginState {
        component: gain.to_le_bytes().to_vec(),
        controller: Vec::new(),
    }
}

/// Creates a processing chain of fake plugins, each restored to the given state for every file.
pub fn test_chain(plugins: &[(Kind, PluginState)]) -> ProcessingChain {
    let plugins = plugins.to_vec();
    dispatcher()
        .call(
            move |ui| {
                let module = Module::from_factory(ui, "test plugins", audiobatch_test_plugin::factory());
                let instances = plugins
                    .into_iter()
                    .map(|(kind, state)| {
                        let class_id = ClassId::from_tuid(&kind.class_id());
                        PluginInstance::create(ui, &module, class_id, Some(dispatcher().clone()))
                            .map(|instance| ChainPlugin::new(kind.name().to_owned(), instance, state))
                            .map_err(|error| error.to_string())
                    })
                    .collect::<Result<Vec<_>, String>>()?;
                Ok::<_, String>(ProcessingChain::new(ui, dispatcher().clone(), instances))
            },
            &|| false,
        )
        .expect("the test UI thread runs the job")
        .expect("the fake plugins are created")
}

/// Copies a fixture into the directory under the given name and analyzes the copy,
/// failing the test when the analysis fails.
pub fn ready_fixture_copy(directory: &TempDir, fixture: &str, copy_name: &str) -> AnalysisRecord {
    let path = test_fixtures::copy_fixture(directory, fixture, Some(copy_name));
    let record = analyze_file(&path, &|| false, &mut |_| {});
    assert!(record.is_ready(), "{}", record.error_message);
    record
}

/// Returns processing options whose disposer deletes files instead of moving them to the trash.
pub fn deleting_options() -> ProcessingOptions {
    ProcessingOptions {
        dispose: Arc::new(|path: &Path| fs::remove_file(path).is_ok() || !path.exists()),
        ..ProcessingOptions::default()
    }
}

/// Decodes a whole file.
pub fn decode(path: &Path) -> DecodedAudio {
    let mut decoder = AudioFileDecoder::open(path).expect("the file decodes");
    let channels = decoder.info().channels;
    let sample_rate = decoder.info().sample_rate;
    let mut samples: Vec<Vec<f32>> = vec![Vec::new(); channels as usize];
    while let Some(chunk) = decoder.next_chunk().expect("the file decodes") {
        for (target, chunk_samples) in samples.iter_mut().zip(chunk) {
            target.extend_from_slice(chunk_samples);
        }
    }
    DecodedAudio {
        channels,
        sample_rate,
        samples,
    }
}

/// Returns the names of the files in a directory, sorted.
pub fn file_names(directory: &TempDir) -> Vec<String> {
    let mut names: Vec<String> = fs::read_dir(directory.path())
        .expect("the directory is readable")
        .filter_map(Result::ok)
        .map(|entry| entry.file_name().to_string_lossy().into_owned())
        .collect();
    names.sort();
    names
}
