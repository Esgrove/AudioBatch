//! Native `RTSan` contracts for the complete preview chain, with plugin lifecycle on its UI thread.

#![cfg(audiobatch_rtsan)]

use std::path::PathBuf;
use std::sync::OnceLock;
use std::thread;

use audiobatch_plugins::{PreviewChain, prepare_for_preview};
use audiobatch_test_plugin::{GAIN_PARAMETER, Kind};
use vst3_host::{ClassId, Module, PluginInstance, UiDispatcher, UiThread};

#[cfg(target_os = "macos")]
mod loaded_runtime {
    //! Inspects dyld's loaded images without entering or modifying sanitizer runtime state.

    #![expect(
        unsafe_code,
        reason = "copies dyld image names while modules remain loaded for the process lifetime"
    )]

    use std::ffi::{CStr, c_char};
    use std::path::PathBuf;

    unsafe extern "C" {
        /// Returns the number of images currently registered with dyld.
        fn _dyld_image_count() -> u32;
        /// Returns a process-owned image name, or null when the index is no longer valid.
        fn _dyld_get_image_name(index: u32) -> *const c_char;
    }

    /// Requires exactly the expected startup runtime before and after native plugin processing.
    /// Call outside realtime regions, with no concurrent module unloading.
    pub fn verify(phase: &str) {
        let expected = PathBuf::from(std::env::var_os("AUDIOBATCH_RTSAN_RUNTIME").expect("explicit startup runtime"))
            .canonicalize()
            .expect("startup runtime exists");
        // SAFETY: querying dyld's image count has no pointer preconditions.
        let count = unsafe { _dyld_image_count() };
        let runtimes: Vec<PathBuf> = (0..count)
            .filter_map(|index| {
                // SAFETY: dyld accepts all indices and returns null for invalid ones.
                let name = unsafe { _dyld_get_image_name(index) };
                if name.is_null() {
                    return None;
                }
                // SAFETY: dyld owns this null-terminated name, and no module is unloaded during this snapshot.
                let name = unsafe { CStr::from_ptr(name) }.to_str().expect("UTF-8 image name");
                let path = PathBuf::from(name);
                path.file_name()
                    .is_some_and(|name| name.to_string_lossy().contains("rtsan"))
                    .then(|| path.canonicalize().expect("loaded runtime exists"))
            })
            .collect();
        assert_eq!(
            runtimes.as_slice(),
            std::slice::from_ref(&expected),
            "exactly the matching startup runtime must be loaded at {phase}"
        );
        println!(
            "AUDIOBATCH_RTSAN_RUNTIME_LOADED phase={phase} count=1 path={}",
            expected.display()
        );
    }
}

/// Block size deliberately smaller than the host's maximum scratch storage.
const BLOCK: usize = 64;
/// One dispatcher owns all plugin lifecycle work in this process.
static DISPATCHER: OnceLock<UiDispatcher> = OnceLock::new();

/// Returns the dedicated native UI dispatcher.
fn dispatcher() -> &'static UiDispatcher {
    DISPATCHER.get_or_init(|| UiThread::spawn_dedicated().expect("native UI thread"))
}

/// Live chain paired with its owning instance until callback retirement on the UI thread.
struct ChainFixture {
    instance: PluginInstance,
    chain: PreviewChain,
}

/// Builds a preview chain without processing or prewarming it, and queues its first automation edit.
fn chain(kind: Option<Kind>) -> ChainFixture {
    dispatcher()
        .call(
            move |ui| {
                let module = if kind.is_some() {
                    Module::from_factory(ui, "RTSan fixture", audiobatch_test_plugin::factory())
                } else {
                    let path =
                        PathBuf::from(std::env::var_os("AUDIOBATCH_TEST_VST3").expect("explicit build-tree plugin"));
                    let root = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
                        .join("../../..")
                        .canonicalize()
                        .expect("repository");
                    let path = path.canonicalize().expect("built native bundle");
                    assert!(
                        path.starts_with(root.join("test-plugin"))
                            || path.starts_with(root.join("image-control-plugin")),
                        "only repository build-tree plugins are allowed"
                    );
                    Module::load(ui, &path).expect("native plugin loads")
                };
                let id = kind.map_or_else(
                    || {
                        module
                            .classes()
                            .iter()
                            .find(|class| class.is_audio_effect())
                            .expect("audio effect")
                            .id
                    },
                    |kind| ClassId::from_tuid(&kind.class_id()),
                );
                let mut instance =
                    PluginInstance::create(ui, &module, id, Some(dispatcher().clone())).expect("instance");
                prepare_for_preview(ui, &mut instance, 48_000.0, BLOCK).expect("realtime setup");
                let chain = PreviewChain::new(ui, &[("RTSan fixture", &instance)]).expect("preview chain");
                instance
                    .set_parameter(ui, GAIN_PARAMETER, 0.25)
                    .expect("first unflushed automation");
                ChainFixture { instance, chain }
            },
            &|| false,
        )
        .expect("UI job")
}

/// Processes on a fresh callback thread, returns ownership before all teardown on the UI thread.
fn process_case(kind: Option<Kind>, reset: bool) {
    #[cfg(target_os = "macos")]
    loaded_runtime::verify("before-plugin");
    let ChainFixture { instance, mut chain } = chain(kind);
    #[cfg(target_os = "macos")]
    loaded_runtime::verify("after-plugin");
    let mut left = [0.5_f32; BLOCK];
    let mut right = [-0.25_f32; BLOCK];
    thread::scope(|scope| {
        scope
            .spawn(|| {
                if reset {
                    chain.reset(0);
                }
                assert!(chain.process(&mut left, &mut right));
            })
            .join()
            .expect("callback thread");
    });
    #[cfg(target_os = "macos")]
    loaded_runtime::verify("after-processing");
    assert!(chain.failure().message().is_none());
    assert!(left.iter().chain(&right).all(|sample| sample.is_finite()));
    if kind == Some(Kind::Gain) {
        assert!(left.iter().all(|sample| (*sample - 0.125).abs() < f32::EPSILON));
        assert!(right.iter().all(|sample| (*sample + 0.0625).abs() < f32::EPSILON));
    }
    if kind.is_none()
        && PathBuf::from(std::env::var_os("AUDIOBATCH_TEST_VST3").expect("native bundle"))
            .file_name()
            .is_some_and(|name| name == "AudioBatch Test MS.vst3")
    {
        // Normalized gain 0.25 is linear gain 0.5, with unity width and zero latency.
        assert!(left.iter().all(|sample| (*sample - 0.25).abs() < f32::EPSILON));
        assert!(right.iter().all(|sample| (*sample + 0.125).abs() < f32::EPSILON));
    }
    dispatcher()
        .call(
            move |ui| {
                drop(chain);
                let mut instance = instance;
                instance.deactivate(ui).expect("deactivate after callback retirement");
                drop(instance);
            },
            &|| false,
        )
        .expect("UI teardown");
}

#[test]
fn preview_gain_first_automation() {
    process_case(Some(Kind::Gain), false);
}

#[test]
fn preview_gain_reset() {
    process_case(Some(Kind::Gain), true);
}

#[cfg(target_os = "macos")]
#[test]
fn preview_native_loaded_runtime_identity() {
    loaded_runtime::verify("before-plugin");
    let ChainFixture { instance, chain } = chain(None);
    loaded_runtime::verify("after-plugin");
    dispatcher()
        .call(
            move |ui| {
                drop(chain);
                let mut instance = instance;
                instance.deactivate(ui).expect("deactivate native runtime probe");
                drop(instance);
            },
            &|| false,
        )
        .expect("UI teardown");
}

#[test]
fn preview_native_first_automation() {
    process_case(None, false);
}

#[test]
fn preview_native_reset() {
    process_case(None, true);
}
