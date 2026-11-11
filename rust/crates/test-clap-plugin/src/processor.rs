//! Allocation-free float32 mid/side processing and a preallocated, activation-latched delay.

use std::sync::Arc;
use std::sync::atomic::Ordering;

use clack_extensions::latency::HostLatency;
use clack_extensions::params::PluginAudioProcessorParams;
use clack_extensions::tail::{PluginTailImpl, TailLength};
use clack_plugin::prelude::*;

use crate::{MainThread, ParameterState, Shared};

/// Audio-thread-owned delay memory, allocated only at activation and released at deactivation.
pub struct AudioProcessor<'a> {
    state: Arc<ParameterState<'a>>,
    channels: usize,
    delay: Box<[[f32; 2]]>,
    position: usize,
}

/// One channel with a writable output and an optional separate input, supporting in-place audio safely.
struct Channel<'a> {
    input: Option<&'a [f32]>,
    output: &'a mut [f32],
}

impl AudioProcessor<'_> {
    /// Delays a processed frame through the fixed ring without allocation or synchronization.
    fn delayed(&mut self, mut frame: [f32; 2]) -> [f32; 2] {
        if let Some(slot) = self.delay.get_mut(self.position) {
            std::mem::swap(slot, &mut frame);
            self.position += 1;
            if self.position == self.delay.len() {
                self.position = 0;
            }
        }
        frame
    }
}

impl<'a> PluginAudioProcessor<'a, Shared<'a>, MainThread<'a>> for AudioProcessor<'a> {
    fn activate(
        _host: HostAudioProcessorHandle<'a>,
        main_thread: &MainThread<'a>,
        shared: &'a Shared<'a>,
        _audio_config: PluginAudioConfiguration,
    ) -> Result<Self, PluginError> {
        let state = Arc::clone(&shared.state);
        let latency = state.latency.load(Ordering::Relaxed);
        let processor = Self {
            state,
            channels: main_thread.channels.get() as usize,
            delay: vec![[0.0; 2]; latency as usize].into_boxed_slice(),
            position: 0,
        };
        let previous_latency = processor.state.active_latency.swap(latency, Ordering::Relaxed);
        processor.state.active.store(true, Ordering::Relaxed);
        if previous_latency != latency
            && let Some(extension) = main_thread.host.get_extension::<HostLatency>()
        {
            extension.changed(&main_thread.host);
        }
        Ok(processor)
    }

    fn process(&mut self, _process: Process, mut audio: Audio, events: Events) -> Result<ProcessStatus, PluginError> {
        let mut changes = events.input.iter().peekable();
        let Some(mut port) = (&mut audio).into_iter().next() else {
            self.state.flush(events.input);
            return Ok(ProcessStatus::Continue);
        };
        let channels = port
            .channels()
            .map_err(|_| PluginError::Message("invalid audio buffers"))?
            .into_f32()
            .ok_or(PluginError::Message("the fixture requires float32 audio"))?;
        if channels.input_channel_count() != self.channels || channels.output_channel_count() != self.channels {
            return Err(PluginError::Message(
                "audio buffers do not match the selected port configuration",
            ));
        }
        let mut channels = channels.into_iter();
        let mut left = Channel::new(channels.next())?;
        let mut right = if self.channels == 2 {
            Some(Channel::new(channels.next())?)
        } else {
            None
        };
        for frame_index in 0..left.output.len() {
            while changes
                .peek()
                .is_some_and(|event| event.header().time() as usize <= frame_index)
            {
                if let Some(event) = changes.next() {
                    self.state.apply(event);
                }
            }
            if self.state.fail.load(Ordering::Relaxed) {
                return Err(PluginError::Message("requested test processing failure"));
            }
            let gain = f32::from_bits(self.state.gain.load(Ordering::Relaxed));
            let width = f32::from_bits(self.state.width.load(Ordering::Relaxed));
            let left_input = left.read(frame_index);
            let frame = right.as_ref().map_or([left_input * gain, 0.0], |right| {
                stereo_frame(left_input, right.read(frame_index), gain, width)
            });
            let [left_output, right_output] = self.delayed(frame);
            left.write(frame_index, left_output);
            if let Some(right) = right.as_mut() {
                right.write(frame_index, right_output);
            }
        }
        for event in changes {
            self.state.apply(event);
        }
        Ok(if self.state.sleep.load(Ordering::Relaxed) {
            ProcessStatus::Sleep
        } else {
            ProcessStatus::Continue
        })
    }

    fn deactivate(self, _main_thread: &MainThread<'a>) {
        self.state.active.store(false, Ordering::Relaxed);
    }

    fn reset(&mut self) {
        self.delay.fill([0.0; 2]);
        self.position = 0;
    }
}

impl PluginAudioProcessorParams for AudioProcessor<'_> {
    fn flush(&mut self, input: &InputEvents, _output: &mut OutputEvents) {
        self.state.flush(input);
    }
}

impl PluginTailImpl for AudioProcessor<'_> {
    fn get(&self) -> TailLength {
        TailLength::Finite(0)
    }
}

impl<'a> Channel<'a> {
    /// Requires both directions, avoiding silent acceptance of malformed host buffers.
    const fn new(pair: Option<ChannelPair<'a, f32>>) -> Result<Self, PluginError> {
        match pair {
            Some(ChannelPair::InputOutput(input, output)) => Ok(Self {
                input: Some(input),
                output,
            }),
            Some(ChannelPair::InPlace(output)) => Ok(Self { input: None, output }),
            _ => Err(PluginError::Message("missing input or output audio channel")),
        }
    }

    /// Reads the untouched input sample before any output for this frame is written.
    fn read(&self, index: usize) -> f32 {
        self.input.unwrap_or(self.output).get(index).copied().unwrap_or(0.0)
    }

    /// Stores a sample into the checked host channel.
    fn write(&mut self, index: usize, sample: f32) {
        if let Some(output) = self.output.get_mut(index) {
            *output = sample;
        }
    }
}

/// Preserves each float32 add and multiply of the independent plugin fixture contract.
#[expect(
    clippy::manual_midpoint,
    reason = "the independent float32 fixture specifies this exact operation order"
)]
fn stereo_frame(left: f32, right: f32, gain: f32, width: f32) -> [f32; 2] {
    let mid = (left + right) * 0.5;
    let side = ((left - right) * 0.5) * width;
    [(mid + side) * gain, (mid - side) * gain]
}
