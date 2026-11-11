//! End-to-end CLAP fixture contracts for saved presets, offline rendering, and mixed-format preview chains.
//!
//! The packaged fixture is built by `test.sh --unit`, never installed or discovered in a user plugin folder.
//! Its gain, width, mono, and compensated-latency outputs must cancel against independent expectations.
//! They use the same `fixtures/plugin_expected/` files as the JUCE VST3 fixture.
//! In-process cases exercise cold callback automation, bounded blocks, reset, and failure retirement.

use std::fs;
use std::path::Path;
use std::sync::{Arc, OnceLock};
use std::thread;

use audiobatch_core::analysis::analyze_file;
use audiobatch_core::decode::AudioFileDecoder;
use audiobatch_plugins::library::bundle_modified_ms;
use audiobatch_plugins::{
    ChainPlugin, ChainSlot, HostInstance, HostState, PluginChain, PluginDescription, PluginFormat, PluginId,
    PresetStore, PreviewChain, ProcessingChain, ProcessingOptions, prepare_for_preview, process_file,
};
use audiobatch_test_clap_plugin::{
    GAIN_PARAMETER, LATENCY_PARAMETER, PLUGIN_ID, PROCESS_FAILURE_PARAMETER, WIDTH_PARAMETER,
};
use audiobatch_test_plugin::Kind;
use tempfile::TempDir;
use test_fixtures::{PLUGIN_EXPECTATIONS, PluginExpectation, STEREO_SPLIT_FIXTURE, null_test};
use vst3_host::{ClassId, ProcessMode, UiDispatcher, UiThread};

/// Rate used by the in-process callback contracts.
const SAMPLE_RATE: f64 = 48_000.0;
/// Maximum callback block, deliberately not a power of two.
const MAX_BLOCK_FRAMES: usize = 257;
/// Small delay spanning several of the tested callback blocks.
const DELAY_FRAMES: u32 = 37;
/// Exactly one UI dispatcher owns every native lifecycle operation in this test executable.
static DISPATCHER: OnceLock<UiDispatcher> = OnceLock::new();

/// A preview chain with its instance kept alive until explicit UI-thread retirement.
struct PreviewFixture {
    instance: HostInstance,
    chain: PreviewChain,
}

/// A mixed-format chain with both lifecycle owners retained until UI-thread retirement.
struct MixedPreviewFixture {
    vst3: HostInstance,
    clap: HostInstance,
    chain: PreviewChain,
}

/// Returns the dedicated thread shared by both native formats.
fn dispatcher() -> &'static UiDispatcher {
    DISPATCHER.get_or_init(|| UiThread::spawn_dedicated().expect("shared plugin UI thread"))
}

/// Creates a CLAP instance without native loading, using only the safe in-process entry.
fn in_process_instance(ui: &UiThread) -> HostInstance {
    let module = clap_host::Module::from_entry(
        ui,
        "in-process CLAP fixture",
        audiobatch_test_clap_plugin::entry().expect("in-process CLAP entry"),
    )
    .expect("CLAP module");
    HostInstance::Clap(
        clap_host::PluginInstance::create(
            ui,
            &module,
            PLUGIN_ID.to_str().expect("UTF-8 fixture id"),
            Some(dispatcher().clone()),
        )
        .expect("CLAP instance"),
    )
}

/// Applies plain CLAP values while inactive and captures the resulting opaque state.
fn configured_state(ui: &UiThread, instance: &HostInstance, expectation: &PluginExpectation) -> HostState {
    instance
        .set_parameter(ui, GAIN_PARAMETER.get(), f64::from(expectation.gain))
        .expect("plain gain");
    instance
        .set_parameter(ui, WIDTH_PARAMETER.get(), f64::from(expectation.width))
        .expect("plain width");
    instance
        .set_parameter(ui, LATENCY_PARAMETER.get(), f64::from(expectation.latency))
        .expect("plain latency");
    instance.state(ui).expect("CLAP state flushes inactive edits")
}

/// Loads a repository-built bundle and records its actual class metadata and settings in a saved slot.
fn plugin_slot(bundle: &Path, expectation: &PluginExpectation) -> ChainSlot {
    let bundle = bundle.to_path_buf();
    let expectation = *expectation;
    dispatcher()
        .call(
            move |ui| {
                let module = clap_host::Module::load(ui, &bundle).expect("built CLAP module loads");
                let class = module.classes(ui).first().expect("CLAP effect descriptor");
                assert!(class.is_effect);
                let description = PluginDescription {
                    name: class.name.clone(),
                    vendor: class.vendor.clone(),
                    version: class.version.clone(),
                    category: class.category.clone(),
                    class_id: PluginId::Clap(class.id.clone()),
                    format: PluginFormat::Clap,
                    sdk_version: String::new(),
                    bundle: bundle.clone(),
                    bundle_modified_ms: bundle_modified_ms(&bundle),
                    is_instrument: false,
                    main_input_channels: Some(2),
                    main_output_channels: Some(2),
                };
                let instance = HostInstance::Clap(
                    clap_host::PluginInstance::create(ui, &module, &class.id, Some(dispatcher().clone()))
                        .expect("packaged CLAP instance"),
                );
                let mut slot = ChainSlot::new(description);
                slot.set_state(configured_state(ui, &instance, &expectation))
                    .expect("matching CLAP state");
                assert!(matches!(slot.state(), HostState::Clap(ref bytes) if !bytes.is_empty()));
                assert_eq!(slot.component_state, [] as [u8; 0]);
                assert_eq!(slot.controller_state, [] as [u8; 0]);
                slot
            },
            &|| false,
        )
        .expect("UI slot construction")
}

/// Saves and reloads a complete format-qualified chain using isolated preset storage.
fn through_preset(directory: &TempDir, chain: &PluginChain) -> PluginChain {
    let presets = PresetStore::new(directory.path().join("presets"));
    presets.save("CLAP verification", chain).expect("save preset");
    let loaded = presets.load("CLAP verification").expect("reload preset").chain();
    assert_eq!(&loaded, chain);
    loaded
}

/// Decodes committed expected audio or rendered output into channel planes.
fn decode(path: &Path) -> Vec<Vec<f32>> {
    let mut decoder = AudioFileDecoder::open(path).expect("fixture decodes");
    let mut planes = vec![Vec::new(); decoder.info().channels as usize];
    while let Some(chunk) = decoder.next_chunk().expect("decoded chunk") {
        for (target, samples) in planes.iter_mut().zip(chunk) {
            target.extend_from_slice(samples);
        }
    }
    planes
}

/// Renders a temporary fixture copy through the production offline chain and returns decoded samples.
fn render(directory: &TempDir, chain: &PluginChain, source: &str) -> Vec<Vec<f32>> {
    let saved = chain.clone();
    let processing = dispatcher()
        .call(
            move |ui| ProcessingChain::instantiate(ui, dispatcher(), &saved),
            &|| false,
        )
        .expect("UI instantiate job")
        .expect("saved chain instantiates");
    render_processing(directory, &processing, source)
}

/// Processes one source through reusable native instances using only a temporary output copy.
fn render_processing(directory: &TempDir, processing: &ProcessingChain, source: &str) -> Vec<Vec<f32>> {
    let path = test_fixtures::copy_fixture(directory, source, Some("render.wav"));
    let record = analyze_file(&path, &|| false, &mut |_| {});
    assert!(record.is_ready(), "{}", record.error_message);
    let options = ProcessingOptions {
        dispose: Arc::new(|path| fs::remove_file(path).is_ok() || !path.exists()),
        ..ProcessingOptions::default()
    };
    let result = process_file(&record, &options, processing, &|| false, &mut |_| {});
    assert_eq!(result.error(), None);
    decode(&result.output().expect("committed CLAP output").path)
}

/// Builds a live stereo chain without prewarming the audio processor.
fn preview(latency: u32) -> PreviewFixture {
    dispatcher()
        .call(
            move |ui| {
                let mut instance = in_process_instance(ui);
                instance
                    .set_parameter(ui, LATENCY_PARAMETER.get(), f64::from(latency))
                    .expect("plain delay");
                let _ = instance.state(ui).expect("flush inactive latency");
                prepare_for_preview(ui, &mut instance, SAMPLE_RATE, MAX_BLOCK_FRAMES).expect("prepare CLAP preview");
                let chain = PreviewChain::new(ui, &[("CLAP fixture", &instance)]).expect("CLAP preview chain");
                PreviewFixture { instance, chain }
            },
            &|| false,
        )
        .expect("UI preview setup")
}

/// Returns callback ownership before stopping and destroying native resources on the UI thread.
fn retire(fixture: PreviewFixture) {
    dispatcher()
        .call(
            move |ui| {
                let PreviewFixture { mut instance, chain } = fixture;
                assert!(instance.has_processor_handles());
                drop(chain);
                assert!(!instance.has_processor_handles());
                instance.deactivate(ui).expect("deactivate retired CLAP instance");
                drop(instance);
                clap_host::service_main_thread(ui);
            },
            &|| false,
        )
        .expect("UI retirement");
}

#[test]
fn packaged_clap_presets_cancel_against_all_shared_independent_outputs() {
    let bundles = TempDir::new().expect("isolated bundle tree");
    let bundle = audiobatch_test_clap_plugin::create_bundle(bundles.path(), "NullContract")
        .expect("build the CLAP fixture with cargo build -p audiobatch-test-clap-plugin --example clap_module");
    for expectation in PLUGIN_EXPECTATIONS {
        let directory = TempDir::new().expect("isolated output");
        let slot = plugin_slot(&bundle, &expectation);
        let chain = through_preset(&directory, &PluginChain { slots: vec![slot] });
        let output = render(&directory, &chain, expectation.source);
        let expected = decode(&expectation.expected_path());
        let result = null_test(&expected, &output);
        assert!(result.cancels(), "{}: {result}", expectation.expected);
    }
}

#[test]
fn packaged_clap_default_state_preserves_the_source() {
    let directory = TempDir::new().expect("isolated bundle and output");
    let bundle = audiobatch_test_clap_plugin::create_bundle(directory.path(), "DefaultContract")
        .expect("build CLAP fixture module before testing");
    let mut expectation = PLUGIN_EXPECTATIONS[0];
    expectation.gain = 1.0;
    expectation.width = 1.0;
    let mut slot = plugin_slot(&bundle, &expectation);
    slot.set_state(HostState::Clap(Vec::new()))
        .expect("matching CLAP state");
    let output = render(&directory, &PluginChain { slots: vec![slot] }, STEREO_SPLIT_FIXTURE);
    let source = decode(&test_fixtures::fixture(STEREO_SPLIT_FIXTURE));
    let result = null_test(&source, &output);
    assert!(result.cancels(), "{result}");
}

#[test]
fn in_process_state_roundtrip_preserves_plain_values_and_latched_latency() {
    dispatcher()
        .call(
            |ui| {
                let original = in_process_instance(ui);
                let state = configured_state(ui, &original, &PLUGIN_EXPECTATIONS[4]);
                let mut restored = in_process_instance(ui);
                assert!(restored.set_state(ui, &state).expect("restore CLAP state"));
                assert_eq!(restored.state(ui).expect("capture restored state"), state);
                prepare_for_preview(ui, &mut restored, SAMPLE_RATE, MAX_BLOCK_FRAMES)
                    .expect("prepare restored instance");
                assert_eq!(restored.latency_samples(ui), PLUGIN_EXPECTATIONS[4].latency);
                restored.deactivate(ui).expect("deactivate restored instance");
            },
            &|| false,
        )
        .expect("UI state roundtrip");
}

#[test]
fn mono_and_stereo_layouts_preserve_plain_gain_across_short_blocks() {
    dispatcher()
        .call(
            |ui| {
                for channels in [1, 2] {
                    let mut instance = in_process_instance(ui);
                    instance
                        .configure_main_buses(ui, channels)
                        .expect("CLAP mono/stereo layout");
                    instance
                        .setup(ui, ProcessMode::Offline, SAMPLE_RATE, MAX_BLOCK_FRAMES)
                        .expect("setup");
                    instance
                        .set_parameter(ui, GAIN_PARAMETER.get(), 0.5)
                        .expect("plain gain");
                    let _ = instance.state(ui).expect("flush gain while inactive");
                    instance.activate(ui).expect("activate");
                    let mut processor = instance.processor(ui).expect("processor");
                    assert_eq!(processor.input_channels(), channels);
                    assert_eq!(processor.output_channels(), channels);
                    for frames in [1, 7, 64, MAX_BLOCK_FRAMES] {
                        for channel in 0..channels {
                            processor.input_channel_mut(channel).expect("input plane").fill(0.25);
                        }
                        processor.process(frames).expect("short block");
                        for channel in 0..channels {
                            assert!(
                                processor.output_channel(channel).expect("output plane")[..frames]
                                    .iter()
                                    .all(|sample| (*sample - 0.125).abs() < f32::EPSILON)
                            );
                        }
                    }
                    drop(processor);
                    instance.deactivate(ui).expect("deactivate");
                }
            },
            &|| false,
        )
        .expect("UI layout contract");
}

#[test]
fn preview_latency_is_block_invariant_and_reset_clears_delay_history() {
    for block in [1, 7, 64, MAX_BLOCK_FRAMES] {
        let mut fixture = preview(DELAY_FRAMES);
        assert_eq!(fixture.chain.latency_frames(), DELAY_FRAMES as usize);
        let frames = DELAY_FRAMES as usize + MAX_BLOCK_FRAMES;
        let mut left = vec![0.5; frames];
        let mut right = vec![-0.25; frames];
        thread::scope(|scope| {
            scope
                .spawn(|| {
                    for (left, right) in left.chunks_mut(block).zip(right.chunks_mut(block)) {
                        assert!(fixture.chain.process(left, right));
                    }
                    fixture.chain.reset(12_345);
                    let mut silent_left = [0.0; MAX_BLOCK_FRAMES];
                    let mut silent_right = [0.0; MAX_BLOCK_FRAMES];
                    assert!(fixture.chain.process(&mut silent_left, &mut silent_right));
                    assert!(
                        silent_left
                            .iter()
                            .chain(&silent_right)
                            .all(|sample| sample.abs() < f32::EPSILON)
                    );
                })
                .join()
                .expect("cold callback thread");
        });
        let delay = DELAY_FRAMES as usize;
        assert!(
            left[..delay]
                .iter()
                .chain(&right[..delay])
                .all(|sample| sample.abs() < f32::EPSILON)
        );
        assert!(left[delay..].iter().all(|sample| (*sample - 0.5).abs() < f32::EPSILON));
        assert!(
            right[delay..]
                .iter()
                .all(|sample| (*sample + 0.25).abs() < f32::EPSILON)
        );
        assert!(fixture.chain.failure().message().is_none());
        retire(fixture);
    }
}

#[test]
fn mixed_vst3_clap_preview_chains_apply_both_formats_in_either_order() {
    for clap_first in [false, true] {
        let fixture = dispatcher()
            .call(
                move |ui| {
                    let module = vst3_host::Module::from_factory(ui, "mixed VST3", audiobatch_test_plugin::factory());
                    let mut vst3 = HostInstance::from(
                        vst3_host::PluginInstance::create(
                            ui,
                            &module,
                            ClassId::from_tuid(&Kind::Gain.class_id()),
                            Some(dispatcher().clone()),
                        )
                        .expect("VST3 instance"),
                    );
                    let mut clap = in_process_instance(ui);
                    prepare_for_preview(ui, &mut vst3, SAMPLE_RATE, MAX_BLOCK_FRAMES).expect("prepare VST3");
                    prepare_for_preview(ui, &mut clap, SAMPLE_RATE, MAX_BLOCK_FRAMES).expect("prepare CLAP");
                    let named = if clap_first {
                        [("CLAP", &clap), ("VST3", &vst3)]
                    } else {
                        [("VST3", &vst3), ("CLAP", &clap)]
                    };
                    let chain = PreviewChain::new(ui, &named).expect("mixed-format preview");
                    vst3.set_parameter(ui, audiobatch_test_plugin::GAIN_PARAMETER, 0.5)
                        .expect("VST3 automation");
                    clap.set_parameter(ui, GAIN_PARAMETER.get(), 0.5)
                        .expect("CLAP plain automation");
                    MixedPreviewFixture { vst3, clap, chain }
                },
                &|| false,
            )
            .expect("UI mixed setup");
        let MixedPreviewFixture { vst3, clap, mut chain } = fixture;
        let mut left = [0.5; 7];
        let mut right = [-0.25; 7];
        thread::scope(|scope| {
            scope
                .spawn(|| assert!(chain.process(&mut left, &mut right)))
                .join()
                .expect("mixed callback");
        });
        assert!(left.iter().all(|sample| (*sample - 0.125).abs() < f32::EPSILON));
        assert!(right.iter().all(|sample| (*sample + 0.0625).abs() < f32::EPSILON));
        assert!(chain.failure().message().is_none());
        dispatcher()
            .call(
                move |ui| {
                    drop(chain);
                    let mut vst3 = vst3;
                    let mut clap = clap;
                    vst3.deactivate(ui).expect("retired VST3");
                    clap.deactivate(ui).expect("retired CLAP");
                    drop(vst3);
                    drop(clap);
                    clap_host::service_main_thread(ui);
                },
                &|| false,
            )
            .expect("UI mixed retirement");
    }
}

#[test]
fn mixed_offline_chains_compensate_clap_latency_and_restore_state_between_files() {
    for clap_first in [false, true] {
        let processing = dispatcher()
            .call(
                move |ui| {
                    let module =
                        vst3_host::Module::from_factory(ui, "mixed offline VST3", audiobatch_test_plugin::factory());
                    let mut vst3 = vst3_host::PluginInstance::create(
                        ui,
                        &module,
                        ClassId::from_tuid(&Kind::Gain.class_id()),
                        Some(dispatcher().clone()),
                    )
                    .expect("offline VST3");
                    vst3.configure_main_buses(ui, 2).expect("VST3 stereo buses");
                    vst3.setup(ui, ProcessMode::Offline, SAMPLE_RATE, MAX_BLOCK_FRAMES)
                        .expect("VST3 setup");
                    vst3.activate(ui).expect("VST3 activation");
                    vst3.set_parameter(ui, audiobatch_test_plugin::GAIN_PARAMETER, 0.5)
                        .expect("VST3 half gain");
                    let vst3_state = vst3.state(ui).expect("VST3 saved gain");
                    vst3.deactivate(ui).expect("VST3 inactive before chain construction");
                    let clap = in_process_instance(ui);
                    let mut expectation = PLUGIN_EXPECTATIONS[4];
                    expectation.gain = 1.0;
                    let clap_state = configured_state(ui, &clap, &expectation);
                    let vst3 = ChainPlugin::new("VST3 gain".to_owned(), vst3, vst3_state);
                    let clap = ChainPlugin::new("CLAP width and delay".to_owned(), clap, clap_state);
                    let plugins = if clap_first { vec![clap, vst3] } else { vec![vst3, clap] };
                    ProcessingChain::new(ui, dispatcher().clone(), plugins)
                },
                &|| false,
            )
            .expect("UI mixed offline chain");
        let expectation = &PLUGIN_EXPECTATIONS[4];
        let expected = decode(&expectation.expected_path());
        for _ in 0..2 {
            let directory = TempDir::new().expect("isolated repeated render");
            let output = render_processing(&directory, &processing, expectation.source);
            let result = null_test(&expected, &output);
            assert!(result.cancels(), "mixed chain, CLAP first {clap_first}: {result}");
        }
    }
}

#[test]
fn failed_clap_callback_names_the_stage_and_silences_all_later_blocks() {
    let fixture = preview(0);
    let mut fixture = dispatcher()
        .call(
            move |ui| {
                fixture
                    .instance
                    .set_parameter(ui, PROCESS_FAILURE_PARAMETER.get(), 1.0)
                    .expect("queue process failure");
                fixture
            },
            &|| false,
        )
        .expect("UI failure automation");
    let mut left = [0.5; 7];
    let mut right = [-0.25; 7];
    thread::scope(|scope| {
        scope
            .spawn(|| {
                assert!(fixture.chain.process(&mut left, &mut right));
                assert!(left.iter().chain(&right).all(|sample| sample.abs() < f32::EPSILON));
                left.fill(1.0);
                right.fill(1.0);
                assert!(fixture.chain.process(&mut left, &mut right));
            })
            .join()
            .expect("failing callback");
    });
    assert!(left.iter().chain(&right).all(|sample| sample.abs() < f32::EPSILON));
    let message = fixture.chain.failure().message().expect("latched CLAP failure");
    assert!(message.starts_with("CLAP fixture:"), "{message}");
    retire(fixture);
}
