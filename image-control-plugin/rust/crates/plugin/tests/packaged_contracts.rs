//! Native build-tree VST3 contracts, independent of the in-process factory and installed plugins.

use std::path::{Path, PathBuf};
use std::sync::{OnceLock, mpsc};
use std::thread::{self, JoinHandle};
use std::time::Duration;

use anyhow::{Context as _, Result, ensure};
use image_control_plugin_rs::{COMPONENT_ID, PLUGIN_NAME};
use vst3_host::{ClassId, Module, PluginInstance, PluginState, ProcessMode, UiDispatcher, UiThread};

#[path = "../examples/support/artifacts.rs"]
mod artifacts;

/// One non-editor UI thread owns every module and component lifecycle in this integration process.
static DISPATCHER: OnceLock<UiDispatcher> = OnceLock::new();
/// Independent 48 kHz kernel and partition delay expectation.
const LATENCY_SAMPLES: usize = 4352;
/// A non-partition-aligned host size exercises the wrapper's block adapter.
const BLOCK_FRAMES: usize = 257;
/// Bounds native module loading, processing, and teardown even if an ABI callback stops returning.
const NATIVE_TIMEOUT: Duration = Duration::from_secs(10);

/// Gives a stuck native callback a fatal, bounded test result rather than hanging the test suite.
struct NativeDeadline {
    /// Wakes the watchdog only after the test's plugin ownership has been retired.
    finished: mpsc::Sender<()>,
    /// Joins the watchdog outside all plugin callbacks.
    worker: Option<JoinHandle<()>>,
}

impl NativeDeadline {
    /// Starts a deadline before entering the native module or UI dispatcher.
    fn start() -> Self {
        let (finished, receiver) = mpsc::channel();
        let worker = thread::spawn(move || {
            if receiver.recv_timeout(NATIVE_TIMEOUT) == Err(mpsc::RecvTimeoutError::Timeout) {
                eprintln!("packaged VST3 contract exceeded its internal 10-second deadline");
                std::process::abort();
            }
        });
        Self {
            finished,
            worker: Some(worker),
        }
    }
}

impl Drop for NativeDeadline {
    fn drop(&mut self) {
        let _ = self.finished.send(());
        if let Some(worker) = self.worker.take() {
            let _ = worker.join();
        }
    }
}

/// Requires an explicitly selected repository bundle. Missing local fixtures skip visibly, but CI never skips.
fn bundle() -> Option<PathBuf> {
    let Some(bundle) = std::env::var_os("IMAGE_CONTROL_OWN_BUNDLE") else {
        assert!(
            std::env::var_os("CI").is_none(),
            "CI must provide IMAGE_CONTROL_OWN_BUNDLE for actual packaged VST3 tests"
        );
        eprintln!("SKIP packaged VST3 contract: set IMAGE_CONTROL_OWN_BUNDLE to an absolute build-tree .vst3 bundle");
        return None;
    };
    let bundle = artifacts::checked_artifact(&PathBuf::from(bundle))
        .expect("IMAGE_CONTROL_OWN_BUNDLE must be an authorized absolute build-tree artifact");
    assert!(
        bundle.extension().is_some_and(|extension| extension == "vst3") && bundle.is_dir(),
        "a real packaged .vst3 directory is required"
    );
    Some(bundle)
}

/// Runs non-editor lifecycle work on one registered host UI thread, never using a fake factory.
fn on_ui<R: Send + 'static>(operation: impl FnOnce(&UiThread) -> Result<R> + Send + 'static) -> R {
    DISPATCHER
        .get_or_init(|| UiThread::spawn_dedicated().expect("packaged host UI thread"))
        .call(operation, &|| false)
        .expect("packaged UI job")
        .expect("packaged contract")
}

/// Loads the actual module and selects its independent persisted component identity.
fn instance(ui: &UiThread, bundle: &Path) -> Result<PluginInstance> {
    let module = Module::load(ui, bundle).context("load build-tree VST3 from disk")?;
    let id = ClassId::from_tuid(&COMPONENT_ID);
    let class = module.class(id).context("primary component class")?;
    ensure!(
        class.is_audio_effect() && class.name == PLUGIN_NAME,
        "wrong packaged plugin identity"
    );
    let mut instance = PluginInstance::create(ui, &module, id, None)?;
    let layout = instance.configure_main_buses(ui, 2)?;
    ensure!(
        layout.inputs == [2] && layout.outputs == [2],
        "matching stereo main buses"
    );
    instance.setup(ui, ProcessMode::Realtime, 48_000.0, BLOCK_FRAMES)?;
    Ok(instance)
}

/// Constructs independently specified protocol bytes rather than asking the implementation to encode its expectations.
fn state_blob(json: &str) -> PluginState {
    let bytes = json.as_bytes();
    let mut component = Vec::with_capacity(bytes.len() + 4);
    component.extend_from_slice(&u32::try_from(bytes.len()).expect("small test state").to_le_bytes());
    component.extend_from_slice(bytes);
    PluginState {
        component,
        controller: Vec::new(),
    }
}

/// Requires the actual packaged neutral path to produce exactly one independently delayed stereo impulse.
#[test]
#[cfg_attr(miri, ignore = "loads and executes a native build-tree VST3")]
fn packaged_neutral_is_bit_exact_at_reported_latency() {
    let Some(bundle) = bundle() else {
        return;
    };
    let _deadline = NativeDeadline::start();
    on_ui(move |ui| {
        let mut instance = instance(ui, &bundle)?;
        instance.activate(ui)?;
        ensure!(
            instance.latency_samples(ui) as usize == LATENCY_SAMPLES,
            "independent latency contract"
        );
        let mut processor = instance.processor(ui)?;
        let worker = thread::spawn(move || -> Result<_> {
            let mut found = None;
            for block in 0..24 {
                for channel in 0..2 {
                    let input = processor.input_channel_mut(channel).context("stereo input")?;
                    input.fill(0.0);
                    if block == 0 {
                        *input.first_mut().context("first input sample")? = if channel == 0 { 0.75 } else { -0.25 };
                    }
                }
                processor.process(BLOCK_FRAMES)?;
                for (frame, (&left, &right)) in processor
                    .output_channel(0)
                    .context("left output")?
                    .iter()
                    .zip(processor.output_channel(1).context("right output")?)
                    .enumerate()
                {
                    if left.to_bits() & 0x7fff_ffff != 0 || right.to_bits() & 0x7fff_ffff != 0 {
                        ensure!(
                            [left.to_bits(), right.to_bits()] == [0.75_f32.to_bits(), (-0.25_f32).to_bits()],
                            "neutral output changed sample bits"
                        );
                        ensure!(
                            found.replace(block * BLOCK_FRAMES + frame).is_none(),
                            "extra nonzero neutral output"
                        );
                    }
                }
            }
            Ok((processor, found))
        });
        let (processor, found) = worker
            .join()
            .map_err(|_| anyhow::anyhow!("native callback thread panicked"))??;
        ensure!(
            found == Some(LATENCY_SAMPLES),
            "impulse is not at independently specified delay"
        );
        drop(processor);
        instance.deactivate(ui)?;
        Ok(())
    });
}

/// Requires automation of every band Mid and Side control to survive packaged native state recall and reset.
#[test]
#[cfg_attr(miri, ignore = "loads and executes a native build-tree VST3")]
fn packaged_band_mutes_survive_state_recall_and_native_reset() {
    let Some(bundle) = bundle() else {
        return;
    };
    let _deadline = NativeDeadline::start();
    on_ui(move |ui| {
        let mut instance = instance(ui, &bundle)?;
        instance.activate(ui)?;
        for band in 0..4_u32 {
            instance.set_parameter(ui, 4 + band * 6, 0.0)?;
            instance.set_parameter(ui, 5 + band * 6, 0.0)?;
        }
        let saved = instance.state(ui)?;
        ensure!(!saved.component.is_empty(), "actual component state is empty");
        instance.deactivate(ui)?;
        ensure!(
            instance.set_state(ui, &saved)?,
            "native state restore refused saved band controls"
        );
        instance.activate(ui)?;
        let mut processor = instance.processor(ui)?;
        let worker = thread::spawn(move || -> Result<_> {
            processor.reset(0);
            for _ in 0..48 {
                processor.input_channel_mut(0).context("left input")?.fill(0.5);
                processor.input_channel_mut(1).context("right input")?.fill(-0.25);
                processor.process(BLOCK_FRAMES)?;
                for channel in 0..2 {
                    ensure!(
                        processor
                            .output_channel(channel)
                            .context("output")?
                            .iter()
                            .all(|sample| sample.abs() < 1e-6),
                        "muted packaged bands produced audio"
                    );
                }
            }
            Ok(processor)
        });
        let processor = worker
            .join()
            .map_err(|_| anyhow::anyhow!("native callback thread panicked"))??;
        drop(processor);
        instance.deactivate(ui)?;
        Ok(())
    });
}

/// Rejects hostile state lengths, incomplete documents, non-finite numbers,
/// and out-of-range normalized values atomically.
#[test]
#[cfg_attr(miri, ignore = "loads a native build-tree VST3")]
fn packaged_untrusted_state_is_rejected_without_mutating_parameters() {
    let Some(bundle) = bundle() else {
        return;
    };
    let _deadline = NativeDeadline::start();
    on_ui(move |ui| {
        let mut instance = instance(ui, &bundle)?;
        let valid = state_blob(r#"{"version":6,"parameters":{"gain":0.25}}"#);
        ensure!(
            instance.set_state(ui, &valid)?,
            "independent current-version state refused"
        );
        let before = instance.state(ui)?.component;
        let invalid = [
            PluginState {
                component: vec![255; 4],
                controller: Vec::new(),
            },
            PluginState {
                component: vec![0; 4],
                controller: Vec::new(),
            },
            PluginState {
                component: vec![8, 0, 0, 0, b'{'],
                controller: Vec::new(),
            },
            state_blob(r#"{"version":99,"parameters":{"gain":0.75}}"#),
            state_blob(r#"{"version":1,"parameters":{"gain":0.75}}"#),
            state_blob(r#"{"version":2,"parameters":{"gain":0.75}}"#),
            state_blob(r#"{"version":3,"parameters":{"gain":0.75}}"#),
            state_blob(r#"{"version":4,"parameters":{"gain":0.75}}"#),
            state_blob(r#"{"version":5,"parameters":{"gain":0.75}}"#),
            state_blob(r#"{"version":6,"parameters":{"gain":0.75},"geometry":{"width":1200,"height":599}}"#),
            state_blob(r#"{"version":6,"parameters":{"gain":0.75},"geometry":{"width":4097,"height":900}}"#),
            state_blob(r#"{"version":6,"parameters":{"gain":0.75},"geometry":{"width":1200,"height":null}}"#),
            state_blob(r#"{"version":6,"parameters":{"gain":1.5}}"#),
            state_blob(r#"{"version":6,"parameters":{"gain":1e999}}"#),
            state_blob(r#"{"version":6,"parameters":{"bands":[{"width":0.25},{"recover":2.0},{},{}]}}"#),
            state_blob(r#"{"version":6,"parameters":{"gain":0.75,"unknown_control":0.5}}"#),
        ];
        for state in invalid {
            ensure!(!instance.set_state(ui, &state)?, "hostile packaged state was accepted");
            ensure!(
                instance.state(ui)?.component == before,
                "rejected state partially changed authoritative controls"
            );
        }
        Ok(())
    });
}

/// The packaged codec persists logical geometry across new components without needing native editor dispatch.
#[test]
#[cfg_attr(miri, ignore = "loads a native build-tree VST3")]
fn packaged_geometry_roundtrips_with_independent_instance_and_missing_defaults() {
    let Some(bundle) = bundle() else {
        return;
    };
    let _deadline = NativeDeadline::start();
    on_ui(move |ui| {
        let mut source = instance(ui, &bundle)?;
        let mut destination = instance(ui, &bundle)?;
        let defaults = destination.state(ui)?;
        let geometry = state_blob(
            r#"{"version":6,"parameters":{"gain":0.25},"tap":"Pre","geometry":{"width":1200.5,"height":900.25}}"#,
        );
        ensure!(source.set_state(ui, &geometry)?, "current geometry restore");
        let saved = source.state(ui)?;
        ensure!(destination.state(ui)? == defaults, "geometry leaked between components");
        ensure!(destination.set_state(ui, &saved)?, "new component geometry restore");
        ensure!(destination.state(ui)? == saved, "geometry did not roundtrip");
        let sparse = state_blob(r#"{"version":6,"parameters":{}}"#);
        ensure!(destination.set_state(ui, &sparse)?, "missing geometry default restore");
        ensure!(
            destination.state(ui)? == defaults,
            "missing geometry did not use fixed defaults"
        );
        ensure!(source.state(ui)? == saved, "restore changed another component");
        Ok(())
    });
}

/// Missing parameters restore fixed neutral defaults, not whichever settings a live instance happened to contain.
#[test]
#[cfg_attr(miri, ignore = "loads a native build-tree VST3")]
fn packaged_missing_fields_restore_independent_neutral_defaults() {
    let Some(bundle) = bundle() else {
        return;
    };
    let _deadline = NativeDeadline::start();
    on_ui(move |ui| {
        let mut instance = instance(ui, &bundle)?;
        let defaults = instance.state(ui)?.component;
        ensure!(
            instance.set_state(ui, &state_blob(r#"{"version":6,"parameters":{"gain":0.1,"mono":1.0}}"#))?,
            "changed state"
        );
        ensure!(
            instance.state(ui)?.component != defaults,
            "changed controls were not restored"
        );
        ensure!(
            instance.set_state(ui, &state_blob(r#"{"version":6,"parameters":{}}"#))?,
            "sparse default state"
        );
        ensure!(
            instance.state(ui)?.component == defaults,
            "missing fields did not restore fixed neutral defaults"
        );
        Ok(())
    });
}
