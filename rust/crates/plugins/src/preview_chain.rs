//! The plugin chain the live preview plays through inside the audio callback.
//!
//! It is built on the UI thread from live instances that are already prepared for stereo realtime processing,
//! and from then on only runs their processor handles, so it can move to the audio thread.
//! It processes stereo blocks in place through every plugin in order,
//! with the same channel handling as offline processing.
//! Nothing in `process` or `reset` allocates or locks.
//! A plugin whose `process` call fails is recorded in the chain's `PreviewFailure`,
//! and the chain outputs silence from then on, while the UI thread reports the failure and turns the preview off.
//! The audio thread cannot format the message itself, because formatting allocates.

use std::sync::Arc;
use std::sync::atomic::{AtomicI32, AtomicUsize, Ordering};

use vst3_host::{PluginInstance, ProcessError, ProcessMode, ProcessorHandle, UiThread};

use crate::chain_signal::{ChainSignal, StageFailure};
use crate::processing_chain::{PROCESSING_BLOCK_FRAMES, limited_tail};

/// Channels the preview plays, since the playback engine maps every file to stereo.
pub(crate) const PREVIEW_CHANNELS: usize = 2;

/// Stage index meaning that no plugin has failed.
const NO_FAILURE: usize = usize::MAX;

/// The first plugin of a preview chain whose `process` call failed,
/// recorded on the audio thread and read on the UI thread.
#[derive(Debug)]
pub struct PreviewFailure {
    /// Display names of the plugins in processing order, for the message.
    names: Vec<String>,
    /// Position of the failed plugin, or `NO_FAILURE`.
    stage: AtomicUsize,
    /// Result code the failed plugin returned.
    result: AtomicI32,
}

/// The plugin chain the live preview plays through.
pub struct PreviewChain {
    /// One processor handle per plugin, in processing order.
    processors: Vec<ProcessorHandle>,
    /// The audio passed from plugin to plugin.
    signal: ChainSignal,
    /// Most frames one `process` call takes.
    max_block_frames: usize,
    /// Sum of the plugins' latencies, in frames.
    latency_frames: usize,
    /// Sum of the plugins' tails when the chain was built, each limited, in frames.
    tail_frames: usize,
    /// Where a failed `process` call is recorded.
    failure: Arc<PreviewFailure>,
}

impl PreviewFailure {
    /// Creates the record of a chain of plugins with these names, with no failure.
    const fn new(names: Vec<String>) -> Self {
        Self {
            names,
            stage: AtomicUsize::new(NO_FAILURE),
            result: AtomicI32::new(0),
        }
    }

    /// Returns the failure as "<name>: <error>", or `None` while every plugin processes.
    #[must_use]
    pub fn message(&self) -> Option<String> {
        let stage = self.stage.load(Ordering::Acquire);
        if stage == NO_FAILURE {
            return None;
        }
        let name = self.names.get(stage).map_or("A plugin", String::as_str);
        let error = ProcessError(self.result.load(Ordering::Relaxed));
        Some(format!("{name}: {error}"))
    }

    /// Returns true when a plugin has failed.
    fn has_failed(&self) -> bool {
        self.stage.load(Ordering::Acquire) != NO_FAILURE
    }

    /// Records a failure unless one is recorded already.
    fn record(&self, failure: StageFailure) {
        if self.has_failed() {
            return;
        }
        self.result.store(failure.error.0, Ordering::Relaxed);
        self.stage.store(failure.index, Ordering::Release);
    }
}

impl PreviewChain {
    /// Builds a chain from live instances in processing order, each with its display name.
    /// The instances must be prepared for stereo realtime processing and active.
    ///
    /// # Errors
    ///
    /// Returns "<name>: <error>" for the first plugin that cannot process audio.
    pub fn new(ui: &UiThread, plugins: &[(&str, &PluginInstance)]) -> Result<Self, String> {
        let mut processors = Vec::with_capacity(plugins.len());
        let mut latency_frames = 0;
        for (name, instance) in plugins {
            let processor = instance.processor(ui).map_err(|error| format!("{name}: {error:#}"))?;
            latency_frames += instance.latency_samples(ui) as usize;
            processors.push(processor);
        }
        let max_block_frames = processors
            .iter()
            .map(ProcessorHandle::max_block_frames)
            .min()
            .unwrap_or(PROCESSING_BLOCK_FRAMES)
            .max(1);
        let sample_rate = processors.first().map_or(0.0, ProcessorHandle::sample_rate);
        let tail_frames = chain_tail_frames(ui, plugins.iter().map(|(_, instance)| *instance), sample_rate);
        let names = plugins.iter().map(|(name, _)| (*name).to_string()).collect();
        Ok(Self {
            processors,
            signal: ChainSignal::new(max_block_frames),
            max_block_frames,
            latency_frames,
            tail_frames,
            failure: Arc::new(PreviewFailure::new(names)),
        })
    }

    /// Returns where a failed `process` call is recorded, for the UI thread to check.
    #[must_use]
    pub fn failure(&self) -> Arc<PreviewFailure> {
        Arc::clone(&self.failure)
    }

    /// Returns the most frames one `process` call takes.
    #[must_use]
    pub const fn max_block_frames(&self) -> usize {
        self.max_block_frames
    }

    /// Returns the sum of the plugins' latencies, in frames.
    #[must_use]
    pub const fn latency_frames(&self) -> usize {
        self.latency_frames
    }

    /// Returns the sum of the plugins' limited tails when the chain was built, in frames.
    #[must_use]
    pub const fn tail_frames(&self) -> usize {
        self.tail_frames
    }

    /// Clears the plugins' internal buffers, such as delay lines and reverb tails,
    /// and tells them that playback starts at `position_frames`, so tempo-synced plugins line up with the file.
    pub fn reset(&mut self, position_frames: u64) {
        for processor in &mut self.processors {
            processor.reset(position_frames);
        }
    }

    /// Processes a stereo block in place, at most `max_block_frames` long.
    /// Returns false when the output is not finite.
    /// After a plugin failed, the block is silenced instead.
    pub fn process(&mut self, left: &mut [f32], right: &mut [f32]) -> bool {
        let frames = left.len().min(right.len()).min(self.max_block_frames);
        let (Some(left), Some(right)) = (left.get_mut(..frames), right.get_mut(..frames)) else {
            return true;
        };
        if self.failure.has_failed() {
            left.fill(0.0);
            right.fill(0.0);
            return true;
        }
        self.signal.reset(PREVIEW_CHANNELS);
        for (channel, samples) in [&*left, &*right].into_iter().enumerate() {
            if let Some(target) = self
                .signal
                .channel_mut(channel)
                .and_then(|target| target.get_mut(..frames))
            {
                target.copy_from_slice(samples);
            }
        }
        if let Err(failure) = self.signal.process(&mut self.processors, frames) {
            self.failure.record(failure);
            left.fill(0.0);
            right.fill(0.0);
            return true;
        }
        self.signal.fit_to(PREVIEW_CHANNELS);
        for (channel, samples) in [left, right].into_iter().enumerate() {
            if let Some(source) = self.signal.channel(channel).and_then(|source| source.get(..frames)) {
                samples.copy_from_slice(source);
            }
        }
        self.signal
            .channel(0)
            .into_iter()
            .chain(self.signal.channel(1))
            .flat_map(|channel| channel.iter().take(frames))
            .all(|sample| sample.is_finite())
    }
}

/// Returns the sum of the tails the instances report now, each limited to `MAXIMUM_TAIL_SECONDS`, in frames.
/// Tails can change while a plugin's settings change, so the preview asks again while it plays.
#[must_use]
pub fn chain_tail_frames<'a>(
    ui: &UiThread,
    instances: impl IntoIterator<Item = &'a PluginInstance>,
    sample_rate: f64,
) -> usize {
    instances
        .into_iter()
        .map(|instance| limited_tail(instance.tail_samples(ui), sample_rate) as usize)
        .sum()
}

/// Prepares an instance for stereo realtime processing at a rate and activates it.
/// A plugin that only takes mono is prepared for mono, and the preview folds the stereo signal into it.
///
/// # Errors
///
/// Returns an error when the plugin cannot be deactivated, takes neither stereo nor mono,
/// or refuses the processing setup or the activation.
pub fn prepare_for_preview(
    ui: &UiThread,
    instance: &mut PluginInstance,
    sample_rate: f64,
    block_frames: usize,
) -> anyhow::Result<()> {
    instance.deactivate(ui)?;
    instance
        .configure_main_buses(ui, PREVIEW_CHANNELS)
        .or_else(|_| instance.configure_main_buses(ui, 1))?;
    instance.setup(ui, ProcessMode::Realtime, sample_rate, block_frames)?;
    instance.activate(ui)
}

#[cfg(test)]
mod test_preview_chain {
    use audiobatch_test_plugin::{DELAY_LATENCY, Kind};
    use vst3_host::{ClassId, Module, PluginState};

    use super::*;
    use crate::test_helpers::{dispatcher, gain_state};

    /// Rate the test chains run at.
    const RATE: f64 = 48_000.0;

    /// Longest block the test instances are set up for.
    const SETUP_BLOCK_FRAMES: usize = 256;

    /// Creates fake plugins prepared for stereo realtime processing and builds a preview chain of them.
    fn preview_chain(plugins: &[(Kind, PluginState)]) -> Result<PreviewChain, String> {
        let plugins = plugins.to_vec();
        dispatcher()
            .call(
                move |ui| {
                    let module = Module::from_factory(ui, "test plugins", audiobatch_test_plugin::factory());
                    let mut instances = Vec::new();
                    for (kind, state) in &plugins {
                        let class_id = ClassId::from_tuid(&kind.class_id());
                        let mut instance = PluginInstance::create(ui, &module, class_id, Some(dispatcher().clone()))
                            .map_err(|error| error.to_string())?;
                        instance.set_state(ui, state).map_err(|error| error.to_string())?;
                        prepare_for_preview(ui, &mut instance, RATE, SETUP_BLOCK_FRAMES)
                            .map_err(|error| format!("{error:#}"))?;
                        instances.push((kind.name(), instance));
                    }
                    let named: Vec<(&str, &PluginInstance)> =
                        instances.iter().map(|(name, instance)| (*name, instance)).collect();
                    PreviewChain::new(ui, &named)
                },
                &|| false,
            )
            .expect("the test UI thread runs the job")
    }

    /// Processes constant stereo input in blocks and returns both output channels.
    fn render(chain: &mut PreviewChain, frames: usize, block: usize, input: (f32, f32)) -> (Vec<f32>, Vec<f32>) {
        let mut left_output = Vec::with_capacity(frames);
        let mut right_output = Vec::with_capacity(frames);
        let mut rendered = 0;
        while rendered < frames {
            let length = block.min(frames - rendered);
            let mut left = vec![input.0; length];
            let mut right = vec![input.1; length];
            assert!(chain.process(&mut left, &mut right));
            left_output.extend_from_slice(&left);
            right_output.extend_from_slice(&right);
            rendered += length;
        }
        (left_output, right_output)
    }

    #[test]
    fn latencies_add_up_and_audio_passes_through_every_plugin() {
        let mut chain = preview_chain(&[(Kind::Delay, PluginState::default()), (Kind::Gain, gain_state(0.5))])
            .expect("the chain builds");
        let latency = DELAY_LATENCY as usize;
        assert_eq!(chain.latency_frames(), latency);
        assert_eq!(chain.max_block_frames(), SETUP_BLOCK_FRAMES);
        let (left, right) = render(&mut chain, latency + 100, 48, (1.0, -1.0));
        let (delayed_left, heard_left) = left.split_at(latency);
        assert!(delayed_left.iter().all(|&sample| sample == 0.0));
        assert!(heard_left.iter().all(|&sample| (sample - 0.5).abs() < 1e-6));
        assert!(right.iter().skip(latency).all(|&sample| (sample + 0.5).abs() < 1e-6));
    }

    #[test]
    fn reset_clears_the_delay_lines() {
        let mut chain = preview_chain(&[(Kind::Delay, PluginState::default())]).expect("the chain builds");
        render(&mut chain, 200, 64, (1.0, 1.0));
        chain.reset(0);
        let (left, _) = render(&mut chain, DELAY_LATENCY as usize, 64, (0.0, 0.0));
        assert!(left.iter().all(|&sample| sample == 0.0));
    }

    #[test]
    fn a_mono_plugin_hears_both_channels_and_feeds_both() {
        let mut chain = preview_chain(&[(Kind::MonoOnly, PluginState::default())]).expect("the chain builds");
        let (left, right) = render(&mut chain, 64, 64, (1.0, 0.0));
        assert!(left.iter().chain(&right).all(|&sample| (sample - 0.5).abs() < 1e-6));
    }

    #[test]
    fn non_finite_output_is_reported() {
        let mut chain = preview_chain(&[(Kind::NotFinite, PluginState::default())]).expect("the chain builds");
        let mut left = vec![0.5; 32];
        let mut right = vec![0.5; 32];
        assert!(!chain.process(&mut left, &mut right));
        assert!(chain.failure().message().is_none());
    }

    #[test]
    fn blocks_longer_than_the_setup_are_cut_and_an_empty_chain_passes_audio() {
        let mut chain = preview_chain(&[(Kind::Gain, gain_state(0.5))]).expect("the chain builds");
        let mut left = vec![1.0; SETUP_BLOCK_FRAMES + 10];
        let mut right = vec![1.0; SETUP_BLOCK_FRAMES + 10];
        assert!(chain.process(&mut left, &mut right));
        let (processed, beyond) = left.split_at(SETUP_BLOCK_FRAMES);
        assert!(processed.iter().all(|&sample| (sample - 0.5).abs() < 1e-6));
        assert!(beyond.iter().all(|&sample| (sample - 1.0).abs() < 1e-6));
        let mut empty = preview_chain(&[]).expect("the chain builds");
        assert_eq!(empty.latency_frames(), 0);
        let (left, _) = render(&mut empty, 10, 10, (0.25, 0.25));
        assert!(left.iter().all(|&sample| (sample - 0.25).abs() < 1e-6));
    }

    #[test]
    fn a_failure_names_the_plugin_once() {
        let failure = PreviewFailure::new(vec![String::from("First"), String::from("Second")]);
        assert!(failure.message().is_none());
        failure.record(StageFailure {
            index: 1,
            error: ProcessError(1),
        });
        failure.record(StageFailure {
            index: 0,
            error: ProcessError(2),
        });
        assert_eq!(
            failure.message().as_deref(),
            Some("Second: The plugin failed to process audio (result 1)")
        );
    }
}
