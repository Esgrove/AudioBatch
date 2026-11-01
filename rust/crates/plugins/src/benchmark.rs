//! In-memory offline benchmarks of the real AudioBatch Test MS VST3 and the production chain.
//!
//! Loading, decoding, preparation, allocation, and serialization are outside the measured interval.
//! Both paths include input and output transfers. The chain path uses the production channel adapter.

use std::path::Path;
use std::time::Instant;

use serde::{Deserialize, Serialize};
use vst3_host::{
    InstanceError, Module, ModuleError, PluginInstance, ProcessError, ProcessMode, ProcessorHandle, UiDispatcher,
};

use crate::chain_signal::ChainSignal;
use crate::library::bundle_modified_ms;
use crate::processing_chain::PROCESSING_BLOCK_FRAMES;
use crate::{ChainSlot, PluginChain, PluginDescription, ProcessingChain, ProcessingChainError};

/// Gain and width used by every instance, with independently reproducible binary32 arithmetic.
pub const BENCHMARK_GAIN: f32 = 0.5;
/// Mid/side width used by every instance.
pub const BENCHMARK_WIDTH: f32 = 0.5;

/// Host call with transfers, or the complete production chain with transfers.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum BenchmarkMode {
    /// One host processor, without the chain's channel adapter.
    Host,
    /// The production chain's channel adapter and processors.
    Chain,
}

/// A malformed request or failure of the real plugin, never a successful skipped benchmark.
#[derive(Debug, thiserror::Error)]
pub enum BenchmarkError {
    /// The request cannot describe a bounded mono/stereo benchmark.
    #[error("Invalid benchmark request: {0}")]
    Invalid(&'static str),
    /// The bundle could not be loaded.
    #[error(transparent)]
    Module(#[from] ModuleError),
    /// The test plugin could not be configured.
    #[error(transparent)]
    Instance(#[from] InstanceError),
    /// The production chain could not be prepared.
    #[error(transparent)]
    Chain(#[from] ProcessingChainError),
    /// A UI thread operation could not be completed.
    #[error(transparent)]
    Dispatch(#[from] vst3_host::CallError),
    /// An audio processor failed.
    #[error("Plugin {stage} failed: {source}")]
    Process {
        /// Zero-based chain stage.
        stage: usize,
        /// VST3 result.
        #[source]
        source: ProcessError,
    },
}

/// Explicit workload settings, identical in both native hosts.
#[derive(Debug, Clone, Copy)]
pub struct BenchmarkOptions {
    /// Timed processing boundary.
    pub mode: BenchmarkMode,
    /// Number of active plugin instances.
    pub instances: usize,
    /// Reported latency per plugin in frames.
    pub latency: u32,
    /// Measured repetitions, after one unreported warmup.
    pub iterations: usize,
}

/// Work geometry and individual timed samples, excluding setup and file I/O.
#[derive(Debug, Serialize, Deserialize)]
pub struct BenchmarkReport {
    /// Native host name.
    pub host: String,
    /// Timed processing boundary.
    pub mode: BenchmarkMode,
    /// Input and output channels.
    pub channels: usize,
    /// Frames per second.
    pub sample_rate: u32,
    /// Actual source frames.
    pub input_frames: usize,
    /// Timed frames including latency drain and final block padding.
    pub rendered_frames: usize,
    /// Frames per processing call.
    pub block_frames: usize,
    /// Active plugin count.
    pub instances: usize,
    /// Total compensated chain latency.
    pub latency_frames: usize,
    /// Gain per instance.
    pub gain: f32,
    /// Width per instance.
    pub width: f32,
    /// One elapsed time per measured repetition.
    pub seconds: Vec<f64>,
}

/// Samples retained for independent checks and serialization after timing.
pub struct BenchmarkOutput {
    /// Work geometry and timings.
    pub report: BenchmarkReport,
    /// Latency-compensated planar output, with the original frame count.
    pub samples: Vec<Vec<f32>>,
}

/// Runs already decoded planes through the real test bundle without file I/O in the timed region.
///
/// # Errors
///
/// Returns an error for malformed planes/settings, a missing or different plugin, or any processing failure.
pub fn benchmark(
    dispatcher: &UiDispatcher,
    bundle: &Path,
    planes: &[Vec<f32>],
    sample_rate: u32,
    options: BenchmarkOptions,
) -> Result<BenchmarkOutput, BenchmarkError> {
    validate(planes, sample_rate, options)?;
    let channels = planes.len();
    let frames = planes.first().expect("validated channels").len();
    let latency = options.instances * options.latency as usize;
    let rendered_frames = (frames + latency).div_ceil(PROCESSING_BLOCK_FRAMES) * PROCESSING_BLOCK_FRAMES;
    let chain = load_chain(dispatcher, bundle, sample_rate, channels, options)?;
    let mut signal = ChainSignal::new(PROCESSING_BLOCK_FRAMES);
    let mut output = vec![vec![0.0; rendered_frames]; channels];
    let mut reference = None;
    let mut seconds = Vec::with_capacity(options.iterations);
    for iteration in 0..=options.iterations {
        let mut prepared = chain.prepare(channels, f64::from(sample_rate), &|| false)?;
        if prepared.latency_samples != latency as u64 || !prepared.refused_settings.is_empty() {
            return Err(BenchmarkError::Invalid("reported latency differs from the request"));
        }
        let start = Instant::now();
        for position in (0..rendered_frames).step_by(PROCESSING_BLOCK_FRAMES) {
            signal.reset(channels);
            for (channel, plane) in planes.iter().enumerate() {
                if position < frames {
                    let input = plane
                        .get(position..(position + PROCESSING_BLOCK_FRAMES).min(frames))
                        .expect("bounded source block");
                    signal
                        .channel_mut(channel)
                        .expect("validated channel")
                        .get_mut(..input.len())
                        .expect("bounded block")
                        .copy_from_slice(input);
                }
            }
            process_block(options.mode, &mut signal, &mut prepared.processors, channels)?;
            for (channel, output) in output.iter_mut().enumerate() {
                output
                    .get_mut(position..position + PROCESSING_BLOCK_FRAMES)
                    .expect("allocated block")
                    .copy_from_slice(signal.channel(channel).expect("validated channel"));
            }
        }
        let elapsed = start.elapsed().as_secs_f64();
        drop(prepared);
        chain.release(&|| false);
        if output.iter().flatten().any(|sample| !sample.is_finite()) {
            return Err(BenchmarkError::Invalid("plugin output is not finite"));
        }
        if let Some(reference) = &reference {
            if &output != reference {
                return Err(BenchmarkError::Invalid(
                    "repeated renders differ after state restoration",
                ));
            }
        } else {
            reference = Some(output.clone());
        }
        if iteration > 0 {
            seconds.push(elapsed);
        }
    }
    let samples = output
        .into_iter()
        .map(|plane| {
            plane
                .get(latency..latency + frames)
                .expect("rendered latency and source")
                .to_vec()
        })
        .collect();
    Ok(BenchmarkOutput {
        report: BenchmarkReport {
            host: "rust".into(),
            mode: options.mode,
            channels,
            sample_rate,
            input_frames: frames,
            rendered_frames,
            block_frames: PROCESSING_BLOCK_FRAMES,
            instances: options.instances,
            latency_frames: latency,
            gain: BENCHMARK_GAIN,
            width: BENCHMARK_WIDTH,
            seconds,
        },
        samples,
    })
}

/// Processes a prepared block with the host transfers or the production channel adapter and finite checks.
fn process_block(
    mode: BenchmarkMode,
    signal: &mut ChainSignal,
    processors: &mut [ProcessorHandle],
    channels: usize,
) -> Result<(), BenchmarkError> {
    match mode {
        BenchmarkMode::Host => {
            let processor = processors.first_mut().expect("one host instance");
            processor
                .process_in_place(&mut signal.channels_mut(), PROCESSING_BLOCK_FRAMES)
                .map_err(|source| BenchmarkError::Process { stage: 0, source })?;
        }
        BenchmarkMode::Chain => {
            signal
                .process(processors, PROCESSING_BLOCK_FRAMES)
                .map_err(|failure| BenchmarkError::Process {
                    stage: failure.index,
                    source: failure.error,
                })?;
            signal.fit_to(channels);
            if (0..channels).any(|channel| {
                signal
                    .channel(channel)
                    .expect("validated channel")
                    .iter()
                    .any(|sample| !sample.is_finite())
            }) {
                return Err(BenchmarkError::Invalid("plugin output is not finite"));
            }
        }
    }
    Ok(())
}

/// Loads the real bundle and captures its automated state before constructing the production chain.
fn load_chain(
    dispatcher: &UiDispatcher,
    bundle: &Path,
    rate: u32,
    channels: usize,
    options: BenchmarkOptions,
) -> Result<ProcessingChain, BenchmarkError> {
    let bundle = bundle.to_path_buf();
    let dispatch = dispatcher.clone();
    dispatcher.call(
        move |ui| {
            let module = Module::load(ui, &bundle)?;
            let class = module
                .classes()
                .iter()
                .find(|class| class.is_audio_effect() && class.name == "AudioBatch Test MS")
                .ok_or(BenchmarkError::Invalid("bundle does not contain AudioBatch Test MS"))?;
            let mut instance = PluginInstance::create(ui, &module, class.id, Some(dispatch.clone()))?;
            instance.configure_main_buses(ui, channels)?;
            instance.setup(ui, ProcessMode::Offline, f64::from(rate), PROCESSING_BLOCK_FRAMES)?;
            instance.activate(ui)?;
            instance.set_parameter(ui, 0, f64::from(BENCHMARK_GAIN * 0.5))?;
            instance.set_parameter(ui, 1, f64::from(BENCHMARK_WIDTH * 0.5))?;
            instance.set_parameter(ui, 2, f64::from(options.latency) / 1024.0)?;
            instance.flush_parameter_edits(ui)?;
            let state = instance.state(ui)?;
            instance.deactivate(ui)?;
            let mut slot = ChainSlot::new(PluginDescription::from_class(
                class,
                &bundle,
                bundle_modified_ms(&bundle),
            ));
            slot.set_state(state);
            let saved = PluginChain {
                slots: vec![slot; options.instances],
            };
            Ok::<_, BenchmarkError>(ProcessingChain::instantiate(ui, &dispatch, &saved)?)
        },
        &|| false,
    )?
}

/// Bounds the benchmark before any native plugin is loaded or large output storage is allocated.
fn validate(planes: &[Vec<f32>], rate: u32, options: BenchmarkOptions) -> Result<(), BenchmarkError> {
    let frames = planes.first().map_or(0, Vec::len);
    if !(1..=2).contains(&planes.len())
        || !(16..=2_822_400).contains(&rate)
        || frames == 0
        || frames > 32_000_000
        || planes
            .iter()
            .any(|plane| plane.len() != frames || plane.iter().any(|sample| !sample.is_finite()))
    {
        return Err(BenchmarkError::Invalid(
            "need finite, equally sized mono/stereo planes, at most 32000000 frames",
        ));
    }
    if !(1..=8).contains(&options.instances)
        || options.latency > 1024
        || !(1..=1000).contains(&options.iterations)
        || (options.mode == BenchmarkMode::Host && options.instances != 1)
    {
        return Err(BenchmarkError::Invalid(
            "need 1..8 instances, 0..1024 latency, 1..1000 iterations, one host instance",
        ));
    }
    Ok(())
}

#[cfg(test)]
mod test_requests {
    use super::*;

    /// A valid small request whose checks do not load a plugin.
    fn options() -> BenchmarkOptions {
        BenchmarkOptions {
            mode: BenchmarkMode::Host,
            instances: 1,
            latency: 0,
            iterations: 2,
        }
    }

    #[test]
    fn accepts_bounded_mono_and_stereo() {
        assert!(validate(&[vec![0.5; 3]], 48_000, options()).is_ok());
        assert!(validate(&[vec![0.5; 3], vec![0.0; 3]], 96_000, options()).is_ok());
    }

    #[test]
    fn rejects_empty_mismatched_and_nonfinite_planes() {
        for planes in [vec![], vec![vec![]], vec![vec![0.0], vec![]], vec![vec![f32::NAN]]] {
            assert!(validate(&planes, 48_000, options()).is_err());
        }
    }

    #[test]
    fn rejects_invalid_work_geometry() {
        for request in [
            BenchmarkOptions {
                instances: 2,
                ..options()
            },
            BenchmarkOptions {
                iterations: 0,
                ..options()
            },
            BenchmarkOptions {
                latency: 1025,
                ..options()
            },
        ] {
            assert!(validate(&[vec![0.0]], 48_000, request).is_err());
        }
    }
}
