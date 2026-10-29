//! Null tests of the `AudioBatch Test MS` VST3 that `test-plugin/` builds with JUCE, the plugin the C++ tests load too.
//!
//! Each test loads the real bundle, sets the gain, width, and latency parameters the way a host automates them,
//! captures the plugin state, saves it in a chain preset, and loads the preset back.
//! The loaded chain then processes a committed fixture,
//! and the output must cancel to the last 16-bit step against the expected file
//! that `fixtures/plugin_expected.py` computes independently.
//! The C++ tests compare against the same files, so passing both means the applications agree with each other.
//!
//! The bundle is built with `CMake`, so a plain `cargo test` may not have it.
//! The tests then skip with a message, except in CI, where a missing bundle fails them.

use std::fs;
use std::path::{Path, PathBuf};
use std::sync::{Arc, OnceLock};

use audiobatch_core::analysis::analyze_file;
use audiobatch_core::decode::AudioFileDecoder;
use audiobatch_plugins::library::bundle_modified_ms;
use audiobatch_plugins::process_file;
use audiobatch_plugins::{ChainSlot, PluginChain, PluginDescription, PresetStore, ProcessingChain, ProcessingOptions};
use tempfile::TempDir;
use test_fixtures::{PLUGIN_EXPECTATIONS, PluginExpectation, STEREO_SPLIT_FIXTURE, null_test, test_plugin_bundle};
use vst3_host::{Module, PluginInstance, ProcessMode, UiDispatcher, UiThread};

/// Id of the gain parameter of the test plugin.
const GAIN_PARAMETER: u32 = 0;

/// Id of the width parameter of the test plugin.
const WIDTH_PARAMETER: u32 = 1;

/// Id of the latency parameter of the test plugin.
const LATENCY_PARAMETER: u32 = 2;

/// Largest latency of the test plugin in samples, which maps linearly from zero.
const LATENCY_MAXIMUM: u32 = 1024;

/// Largest plain value of both parameters, which map linearly from zero.
const PARAMETER_MAXIMUM: f32 = 2.0;

/// Sample rate the plugin is set up at while its parameters are set.
const SETUP_SAMPLE_RATE: f64 = 44_100.0;

/// Block size the plugin is set up for while its parameters are set.
const SETUP_BLOCK_FRAMES: usize = 512;

/// Parameter values set on the test plugin the way a host automates them.
#[derive(Debug, Clone, Copy)]
struct Parameters {
    /// Linear gain, from 0 to 2.
    gain: f32,
    /// Stereo width, from 0 to 2.
    width: f32,
    /// Latency in samples.
    latency: u32,
}

impl From<&PluginExpectation> for Parameters {
    fn from(expectation: &PluginExpectation) -> Self {
        Self {
            gain: expectation.gain,
            width: expectation.width,
            latency: expectation.latency,
        }
    }
}

/// The dispatcher of the one UI thread these tests share, since a process can have only one.
static DISPATCHER: OnceLock<UiDispatcher> = OnceLock::new();

/// Returns the dispatcher of the shared test UI thread, starting the thread on first use.
fn dispatcher() -> &'static UiDispatcher {
    DISPATCHER.get_or_init(|| UiThread::spawn_dedicated().expect("no other UI thread in the test process"))
}

/// Returns the test plugin bundle, or `None` to skip the test when it has not been built.
///
/// # Panics
///
/// Panics in CI when the bundle is missing, so a broken plugin build cannot pass as skipped tests.
fn bundle() -> Option<PathBuf> {
    let bundle = test_plugin_bundle();
    if bundle.is_none() {
        assert!(
            std::env::var_os("CI").is_none(),
            "The test plugin bundle is missing. Build it with test-plugin/build.sh or set {}",
            test_fixtures::TEST_PLUGIN_VARIABLE
        );
        eprintln!("Skipping: build the test plugin with test-plugin/build.sh to run the JUCE plugin tests");
    }
    bundle
}

/// Loads the plugin from the bundle and returns its chain slot.
/// With parameter values, an instance is created, set to them through the controller as a host does,
/// and the slot gets the state the plugin saves after the change.
fn plugin_slot(bundle: &Path, parameters: Option<Parameters>) -> ChainSlot {
    let bundle = bundle.to_path_buf();
    dispatcher()
        .call(
            move |ui| {
                let module = Module::load(ui, &bundle).map_err(|error| format!("{error:#}"))?;
                let class = module
                    .classes()
                    .iter()
                    .find(|class| class.is_audio_effect())
                    .ok_or("The bundle has no audio effect")?;
                let mut slot = ChainSlot::new(PluginDescription::from_class(
                    class,
                    &bundle,
                    bundle_modified_ms(&bundle),
                ));
                if let Some(parameters) = parameters {
                    let state =
                        configured_state(ui, &module, class.id, parameters).map_err(|error| format!("{error:#}"))?;
                    slot.set_state(state);
                }
                Ok::<_, String>(slot)
            },
            &|| false,
        )
        .expect("the test UI thread runs the job")
        .expect("the test plugin loads")
}

/// Creates an instance, sets its gain, width, and latency, and returns the state it saves after the change.
fn configured_state(
    ui: &UiThread,
    module: &Arc<Module>,
    class_id: vst3_host::ClassId,
    parameters: Parameters,
) -> anyhow::Result<vst3_host::PluginState> {
    let mut instance = PluginInstance::create(ui, module, class_id, None)?;
    instance.configure_main_buses(ui, 2)?;
    instance.setup(ui, ProcessMode::Offline, SETUP_SAMPLE_RATE, SETUP_BLOCK_FRAMES)?;
    instance.activate(ui)?;
    instance.set_parameter(ui, GAIN_PARAMETER, f64::from(parameters.gain / PARAMETER_MAXIMUM))?;
    instance.set_parameter(ui, WIDTH_PARAMETER, f64::from(parameters.width / PARAMETER_MAXIMUM))?;
    instance.set_parameter(
        ui,
        LATENCY_PARAMETER,
        f64::from(parameters.latency) / f64::from(LATENCY_MAXIMUM),
    )?;
    instance.flush_parameter_edits(ui);
    let state = instance.state(ui)?;
    instance.deactivate(ui)?;
    instance.activate(ui)?;
    let reported_latency = instance.latency_samples(ui);
    instance.deactivate(ui)?;
    anyhow::ensure!(
        reported_latency == parameters.latency,
        "the plugin reports {reported_latency} samples of latency instead of {}",
        parameters.latency
    );
    Ok(state)
}

/// Saves the chain as a preset in the directory, loads it back, and returns the loaded chain.
fn through_preset(directory: &TempDir, chain: &PluginChain) -> PluginChain {
    let presets = PresetStore::new(directory.path().join("chain-presets"));
    presets.save("Test MS", chain).expect("the preset is saved");
    let loaded = presets.load("Test MS").expect("the preset loads").chain();
    assert_eq!(&loaded, chain);
    loaded
}

/// Processes a copy of a fixture through a chain and returns the decoded output.
fn process(directory: &TempDir, chain: &PluginChain, fixture: &str, copy_name: &str) -> Vec<Vec<f32>> {
    let saved = chain.clone();
    let processing = dispatcher()
        .call(
            move |ui| ProcessingChain::instantiate(ui, dispatcher(), &saved),
            &|| false,
        )
        .expect("the test UI thread runs the job")
        .expect("the chain is instantiated");
    let source = test_fixtures::copy_fixture(directory, fixture, Some(copy_name));
    let record = analyze_file(&source, &|| false, &mut |_| {});
    assert!(record.is_ready(), "{}", record.error_message);
    let options = ProcessingOptions {
        dispose: Arc::new(|path: &Path| fs::remove_file(path).is_ok() || !path.exists()),
        ..ProcessingOptions::default()
    };
    let result = process_file(&record, &options, &processing, &|| false, &mut |_| {});
    assert_eq!(result.error(), None);
    let output = result.output().expect("the output is committed");
    assert!(
        output
            .overloads
            .as_ref()
            .expect("reanalyzed output overloads")
            .matches_stream(output.sample_rate, output.length_in_samples, output.channels,)
    );
    decode(&output.path)
}

/// Decodes a whole file into its samples per channel.
fn decode(path: &Path) -> Vec<Vec<f32>> {
    let mut decoder = AudioFileDecoder::open(path).expect("the file decodes");
    let mut samples: Vec<Vec<f32>> = vec![Vec::new(); decoder.info().channels as usize];
    while let Some(chunk) = decoder.next_chunk().expect("the file decodes") {
        for (target, chunk_samples) in samples.iter_mut().zip(chunk) {
            target.extend_from_slice(chunk_samples);
        }
    }
    samples
}

/// Runs one expectation: sets the parameters, round trips the chain through a preset,
/// processes the source, and null tests the output against the expected file.
fn check_expectation(bundle: &Path, expectation: &PluginExpectation) {
    let directory = TempDir::new().expect("temp dir");
    let slot = plugin_slot(bundle, Some(Parameters::from(expectation)));
    let chain = through_preset(&directory, &PluginChain { slots: vec![slot] });
    let output = process(
        &directory,
        &chain,
        expectation.source,
        &format!("processed_{}", expectation.source),
    );
    let expected = decode(&expectation.expected_path());
    let result = null_test(&expected, &output);
    assert!(
        result.cancels(),
        "gain {}, width {}, and latency {} on {} against {}: {result}",
        expectation.gain,
        expectation.width,
        expectation.latency,
        expectation.source,
        expectation.expected
    );
}

#[test]
fn the_default_settings_leave_the_audio_untouched() {
    let Some(bundle) = bundle() else {
        return;
    };
    let directory = TempDir::new().expect("temp dir");
    let chain = PluginChain {
        slots: vec![plugin_slot(&bundle, None)],
    };
    let output = process(&directory, &chain, STEREO_SPLIT_FIXTURE, "identity.wav");
    let source = decode(&test_fixtures::fixture(STEREO_SPLIT_FIXTURE));
    let result = null_test(&source, &output);
    assert!(result.cancels(), "{result}");
}

#[test]
fn half_gain_and_half_width_cancel_against_the_expected_output() {
    if let Some(bundle) = bundle() {
        check_expectation(&bundle, &PLUGIN_EXPECTATIONS[0]);
    }
}

#[test]
fn zero_width_folds_to_mono_and_cancels_against_the_expected_output() {
    if let Some(bundle) = bundle() {
        check_expectation(&bundle, &PLUGIN_EXPECTATIONS[1]);
    }
}

#[test]
fn double_width_cancels_against_the_expected_output() {
    if let Some(bundle) = bundle() {
        check_expectation(&bundle, &PLUGIN_EXPECTATIONS[2]);
    }
}

#[test]
fn a_mono_file_gets_only_the_gain_and_cancels_against_the_expected_output() {
    if let Some(bundle) = bundle() {
        check_expectation(&bundle, &PLUGIN_EXPECTATIONS[3]);
    }
}

#[test]
fn a_changed_parameter_is_saved_in_the_plugin_state() {
    let Some(bundle) = bundle() else {
        return;
    };
    let default_slot = plugin_slot(&bundle, None);
    let changed = plugin_slot(&bundle, Some(Parameters::from(&PLUGIN_EXPECTATIONS[0])));
    let unchanged = plugin_slot(
        &bundle,
        Some(Parameters {
            gain: 1.0,
            width: 1.0,
            latency: 0,
        }),
    );
    assert_eq!(default_slot.component_state, [] as [u8; 0]);
    assert_ne!(changed.component_state, [] as [u8; 0]);
    assert_ne!(changed.component_state, unchanged.component_state);
}

#[test]
fn a_latency_is_compensated_and_its_tail_rendered_against_the_expected_output() {
    if let Some(bundle) = bundle() {
        check_expectation(&bundle, &PLUGIN_EXPECTATIONS[4]);
    }
}
