//! Test-only native wet insert for the hardware-free production callback benchmark.
//!
//! Only the repository's built `AudioBatch Test MS` VST3 is accepted.
//! `test_fixtures::test_plugin_bundle` locates it, including an explicit `AUDIOBATCH_TEST_VST3` override.
//! Canonical paths must stay under the repository's `test-plugin` directory, never an installed plugin folder.
//! Gain is 0.5, width is 1.0, and latency is zero, the plugin's initial latency default.
//! Parameter edits remain queued until the first measured process call, with no flush or reactivation prewarm.
//!
//! Module loading, instance setup, parameter publication, insert disposal, and teardown run on one dedicated UI thread.
//! The first callback delivers the queued parameter edits through the production preview chain.
//! The callback receives only a `PreviewChain` adapter, whose trait methods neither log nor dispatch UI work.
//! The fixture owns the live instance through `UiBound`, without extending the playback crate's production dependencies.
//!
//! After callback workers join, detach and settle the insert before collecting every retired insert.
//! Pass those inserts to `NativeFixture::dispose_inserts`, check `NativeFixture::validate` outside callback timing,
//! then call `NativeFixture::finish` to deactivate and destroy the instance synchronously on the UI thread.
//! Dropping the fixture alone preserves UI-bound ownership but does not prove processors have been retired.

use std::fs::{self, File};
use std::io::Read as _;
use std::path::{Path, PathBuf};
use std::sync::{Arc, OnceLock};
use std::thread;

use anyhow::{Context as _, Result, bail, ensure};
use audiobatch_plugins::{PreviewChain, PreviewFailure, prepare_for_preview};
use test_fixtures::test_plugin_bundle;
use vst3_host::{Module, PluginInstance, UiBound, UiDispatcher, UiThread};

use crate::PreviewInsert;

/// Native fixture class, excluding a different effect in an explicitly supplied bundle.
const PLUGIN_NAME: &str = "AudioBatch Test MS";
/// Legacy VST3 parameter id of the test plugin's linear gain.
const GAIN_PARAMETER: u32 = 0;
/// Legacy VST3 parameter id of its mid/side width.
const WIDTH_PARAMETER: u32 = 1;
/// Legacy VST3 parameter id of its reported delay.
const LATENCY_PARAMETER: u32 = 2;
/// Normalized gain corresponding to a linear multiplier of one half.
const NORMALIZED_GAIN: f64 = 0.25;
/// Normalized width corresponding to unchanged stereo width.
const NORMALIZED_WIDTH: f64 = 0.5;
/// One process-wide UI thread, since VST3 UI thread registration cannot move between libtest threads.
static DISPATCHER: OnceLock<UiDispatcher> = OnceLock::new();

/// Control-side native ownership, retained until every callback processor has been returned and disposed.
pub struct NativeFixture {
    /// Kept alive through synchronous insert disposal and final instance teardown.
    dispatcher: UiDispatcher,
    /// Only accessed through the UI thread token, even when the fixture lives on the control thread.
    instance: UiBound<PluginInstance>,
    /// Read only after processing, since native process failures can otherwise appear as finite silence.
    failure: Arc<PreviewFailure>,
    /// Canonical build-tree path for benchmark provenance.
    bundle: PathBuf,
    /// FNV-1a identity of the native executable bytes, sampled before loading and outside all timing.
    binary_checksum: String,
}

/// The production preview chain presented through the plugin-independent playback trait.
struct NativeInsert(PreviewChain);

/// Native fixture lifetime owner and the insert transferred to the audio callback.
pub struct PreparedNativeFixture {
    pub fixture: NativeFixture,
    pub insert: Box<dyn PreviewInsert>,
}

impl NativeFixture {
    /// Creates an explicitly requested native wet fixture and its callback-owned insert without timing setup.
    /// The returned insert must be retired and disposed before `finish` is called.
    ///
    /// # Errors
    ///
    /// Returns an error for a missing or nonrepository bundle, invalid geometry, failed native setup,
    /// or unexpected latency or tail.
    ///
    /// # Panics
    ///
    /// Panics if a different thread already owns VST3 UI registration or the dedicated thread cannot start.
    pub fn prepare(sample_rate: u32, block_frames: usize) -> Result<PreparedNativeFixture> {
        ensure!(
            sample_rate > 0 && block_frames > 0,
            "Native callback geometry must be nonzero"
        );
        let bundle = repository_bundle()?;
        let binary_checksum = executable_checksum(&bundle)?;
        let dispatcher = DISPATCHER
            .get_or_init(|| UiThread::spawn_dedicated().expect("native callback benchmark UI thread"))
            .clone();
        let setup_dispatcher = dispatcher.clone();
        let setup_bundle = bundle.clone();
        let (instance, chain) = dispatcher
            .call(
                move |ui| {
                    let module = Module::load(ui, &setup_bundle)?;
                    let class = module
                        .classes()
                        .iter()
                        .find(|class| class.is_audio_effect() && class.name == PLUGIN_NAME)
                        .context("Native callback bundle does not contain AudioBatch Test MS")?;
                    let mut instance = PluginInstance::create(ui, &module, class.id, Some(setup_dispatcher.clone()))?;
                    prepare_for_preview(ui, &mut instance, f64::from(sample_rate), block_frames)?;
                    instance.set_parameter(ui, GAIN_PARAMETER, NORMALIZED_GAIN)?;
                    instance.set_parameter(ui, WIDTH_PARAMETER, NORMALIZED_WIDTH)?;
                    instance.set_parameter(ui, LATENCY_PARAMETER, 0.0)?;
                    // Zero is already the latency default, so preparation needs no parameter-delivery process call.
                    // The first callback delivers the queued gain and width edits to the native processor.
                    let chain = PreviewChain::new(ui, &[(PLUGIN_NAME, &instance)])?;
                    ensure!(
                        chain.latency_frames() == 0,
                        "Native callback fixture must have zero latency"
                    );
                    ensure!(chain.tail_frames() == 0, "Native callback fixture must have no tail");
                    Ok::<_, anyhow::Error>((UiBound::new(ui, setup_dispatcher, instance), chain))
                },
                &|| false,
            )
            .context("Native callback setup UI job failed")??;
        let failure = chain.failure();
        Ok(PreparedNativeFixture {
            fixture: Self {
                dispatcher,
                instance,
                failure,
                bundle,
                binary_checksum,
            },
            insert: Box::new(NativeInsert(chain)),
        })
    }

    /// Returns the canonical first-party bundle path for serialization outside callback timing.
    pub fn bundle(&self) -> &Path {
        &self.bundle
    }

    /// Returns the native executable byte identity, explicitly labeled with its noncryptographic hash algorithm.
    pub fn binary_checksum(&self) -> &str {
        &self.binary_checksum
    }

    /// Rejects any native process failure, including failures that made the preview chain emit finite silence.
    /// Call after processing, never from the audio callback.
    ///
    /// # Errors
    ///
    /// Returns the chain's first recorded native processing failure.
    pub fn validate(&self) -> Result<()> {
        if let Some(message) = self.failure.message() {
            bail!("Native callback fixture failed: {message}");
        }
        Ok(())
    }

    /// Destroys retired inserts and their processor handles on the UI thread, outside callback timing.
    /// Only pass inserts returned after callback retirement or removed while the renderer is quiescent.
    /// This can be called between batches as well as during final teardown.
    ///
    /// # Errors
    ///
    /// Returns an error when the UI job cannot complete.
    pub fn dispose_inserts(&self, inserts: Vec<Box<dyn PreviewInsert>>) -> Result<()> {
        self.dispatcher
            .call(move |_| drop(inserts), &|| false)
            .context("Native callback insert disposal UI job failed")
    }

    /// Deactivates and drops the live instance on the UI thread after every insert has been disposed.
    /// Call even when output validation failed, so a failed benchmark still completes native teardown.
    ///
    /// # Errors
    ///
    /// Returns an error if any processor remains alive, deactivation fails, or the UI job cannot complete.
    pub fn finish(self) -> Result<()> {
        let Self {
            dispatcher,
            mut instance,
            failure: _,
            bundle: _,
            binary_checksum: _,
        } = self;
        dispatcher
            .call(
                move |ui| {
                    instance.get_mut(ui).deactivate(ui)?;
                    drop(instance);
                    Ok::<_, anyhow::Error>(())
                },
                &|| false,
            )
            .context("Native callback teardown UI job failed")?
    }
}

impl PreviewInsert for NativeInsert {
    fn max_block_frames(&self) -> usize {
        self.0.max_block_frames()
    }

    fn latency_frames(&self) -> usize {
        self.0.latency_frames()
    }

    fn tail_frames(&self) -> usize {
        self.0.tail_frames()
    }

    fn reset(&mut self, position_frames: u64) {
        self.0.reset(position_frames);
    }

    fn process(&mut self, left: &mut [f32], right: &mut [f32]) -> bool {
        self.0.process(left, right)
    }
}

#[test]
#[ignore = "explicit native build-tree setup and correctness contract, not a performance measurement"]
fn native_preview_setup_processing_and_teardown() {
    let PreparedNativeFixture { fixture, mut insert } = NativeFixture::prepare(48_000, 63).expect("native setup");
    let mut left = [0.5_f32; 63];
    let mut right = [-0.25_f32; 63];
    thread::scope(|scope| {
        scope
            .spawn(|| {
                insert.reset(0);
                assert!(insert.process(&mut left, &mut right));
            })
            .join()
            .expect("native process callback");
    });
    let correct = left.iter().all(|&sample| (sample - 0.25).abs() <= f32::EPSILON)
        && right.iter().all(|&sample| (sample + 0.125).abs() <= f32::EPSILON);
    fixture
        .dispose_inserts(vec![insert])
        .expect("native processor disposal");
    let validation = fixture.validate();
    let teardown = fixture.finish();
    validation.expect("native failure record");
    teardown.expect("native teardown");
    assert!(correct, "native gain and width match independent expected values");
}

/// Hashes the one native executable in the platform's bundle directory before loading it.
/// This is a reproducibility checksum, not a security or authenticity guarantee.
fn executable_checksum(bundle: &Path) -> Result<String> {
    let folders: &[&str] = if cfg!(target_os = "macos") {
        &["MacOS"]
    } else if cfg!(target_arch = "aarch64") {
        &["arm64-win", "arm64x-win"]
    } else {
        &["x86_64-win"]
    };
    let folder = folders
        .iter()
        .map(|folder| bundle.join("Contents").join(folder))
        .find(|folder| folder.is_dir())
        .context("Native callback executable directory is missing")?;
    let mut executables = fs::read_dir(&folder)?
        .map(|entry| entry.map(|entry| entry.path()))
        .collect::<std::io::Result<Vec<_>>>()?;
    executables.retain(|path| {
        path.is_file() && (cfg!(target_os = "macos") || path.extension().is_some_and(|extension| extension == "vst3"))
    });
    ensure!(
        executables.len() == 1,
        "Native callback executable identity must be unambiguous"
    );
    let mut file = File::open(executables.first().context("Native callback executable is missing")?)?;
    let mut buffer = [0_u8; 16_384];
    let mut checksum = 0xcbf2_9ce4_8422_2325_u64;
    loop {
        let length = file.read(&mut buffer)?;
        if length == 0 {
            break;
        }
        for &byte in buffer.get(..length).context("Native checksum read size")? {
            checksum = (checksum ^ u64::from(byte)).wrapping_mul(0x100_0000_01b3);
        }
    }
    Ok(format!("fnv1a64:{checksum:016x}"))
}

/// Locates the explicitly requested benchmark input and rejects installed or otherwise external plugins.
fn repository_bundle() -> Result<PathBuf> {
    let bundle = test_plugin_bundle()
        .context("Native callback benchmark requires the built test plugin or AUDIOBATCH_TEST_VST3")?
        .canonicalize()
        .context("Native callback benchmark bundle could not be resolved")?;
    let repository = Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("../../..")
        .canonicalize()
        .context("Native callback benchmark repository could not be resolved")?;
    require_repository_bundle(&repository, &bundle)?;
    Ok(bundle)
}

/// Restricts an already canonical bundle path to the repository's native test plugin source/build tree.
fn require_repository_bundle(repository: &Path, bundle: &Path) -> Result<()> {
    ensure!(
        bundle.starts_with(repository.join("test-plugin")),
        "Native callback benchmark only accepts bundles in the repository's test-plugin build tree"
    );
    Ok(())
}

#[test]
fn accepts_test_plugin_build_tree() {
    let repository = Path::new("repository");
    let bundle = repository.join("test-plugin/cmake-build-release/AudioBatch Test MS.vst3");
    assert!(require_repository_bundle(repository, &bundle).is_ok());
}

#[test]
fn rejects_external_bundle() {
    let repository = Path::new("repository");
    let bundle = Path::new("installed/AudioBatch Test MS.vst3");
    assert!(require_repository_bundle(repository, bundle).is_err());
}

#[test]
fn rejects_sibling_with_matching_name_prefix() {
    let repository = Path::new("repository");
    let bundle = repository.join("test-plugin-external/AudioBatch Test MS.vst3");
    assert!(require_repository_bundle(repository, &bundle).is_err());
}
