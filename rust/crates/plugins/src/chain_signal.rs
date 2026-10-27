//! Passing one block of audio through every plugin of a chain.
//!
//! The signal starts with the file's channel count and is at most stereo.
//! A mono signal reaching a plugin with a stereo main input is copied to both input channels,
//! so stereo linked and mid/side processing see a centred source instead of a hard left one.
//! A stereo signal reaching a plugin with a mono main input is folded to the average of both channels,
//! so the order of the plugins does not drop a channel.
//! Main input channels beyond the signal get silence.
//! After each plugin the signal has as many channels as the plugin's main output, limited to mono or stereo.

use vst3_host::{ProcessError, ProcessorHandle};

/// Most channels the signal carries.
const MAXIMUM_SIGNAL_CHANNELS: usize = 2;

/// The audio passed from plugin to plugin, one block long.
pub struct ChainSignal {
    /// The channel buffers, of which the first `width` carry signal.
    channels: [Vec<f32>; MAXIMUM_SIGNAL_CHANNELS],
    /// Number of channels carrying signal, 1 or 2.
    width: usize,
}

impl ChainSignal {
    /// Creates a silent mono signal of `frames` frames.
    pub fn new(frames: usize) -> Self {
        Self {
            channels: [vec![0.0; frames], vec![0.0; frames]],
            width: 1,
        }
    }

    /// Silences the block and sets the number of channels carrying signal, limited to mono or stereo.
    pub fn reset(&mut self, width: usize) {
        for channel in &mut self.channels {
            channel.fill(0.0);
        }
        self.width = width.clamp(1, MAXIMUM_SIGNAL_CHANNELS);
    }

    /// Returns a channel for filling the block, or `None` beyond stereo.
    pub fn channel_mut(&mut self, channel: usize) -> Option<&mut [f32]> {
        self.channels.get_mut(channel).map(Vec::as_mut_slice)
    }

    /// Returns a channel of the block, or `None` beyond stereo.
    pub fn channel(&self, channel: usize) -> Option<&[f32]> {
        self.channels.get(channel).map(Vec::as_slice)
    }

    /// Runs the whole block through every processor in order.
    ///
    /// # Errors
    ///
    /// Returns the failure of the first plugin whose `process` call fails.
    pub fn process(&mut self, processors: &mut [ProcessorHandle]) -> Result<(), ProcessError> {
        let frames = self.channels[0].len();
        for processor in processors {
            let main_inputs = processor.input_channels();
            self.fit_to(main_inputs.clamp(1, MAXIMUM_SIGNAL_CHANNELS));
            for channel in 0..main_inputs {
                let Some(input) = processor.input_channel_mut(channel) else {
                    continue;
                };
                match self.channels.get(channel).filter(|_| channel < self.width) {
                    Some(signal) => copy_samples(signal, input),
                    None => input.fill(0.0),
                }
            }
            processor.process(frames)?;
            let main_outputs = processor.output_channels();
            for (channel, signal) in self.channels.iter_mut().enumerate() {
                match processor.output_channel(channel) {
                    Some(output) => copy_samples(output, signal),
                    None => signal.fill(0.0),
                }
            }
            self.width = main_outputs.clamp(1, MAXIMUM_SIGNAL_CHANNELS);
        }
        Ok(())
    }

    /// Changes the number of channels carrying signal,
    /// copying mono to both channels or folding stereo to the average of both.
    pub fn fit_to(&mut self, width: usize) {
        let width = width.clamp(1, MAXIMUM_SIGNAL_CHANNELS);
        let [left, right] = &mut self.channels;
        if self.width == 1 && width == 2 {
            right.copy_from_slice(left);
        } else if self.width == 2 && width == 1 {
            for (left, right) in left.iter_mut().zip(right.iter()) {
                *left = f32::midpoint(*left, *right);
            }
        }
        self.width = width;
    }
}

/// Copies as many samples as both slices hold.
fn copy_samples(source: &[f32], target: &mut [f32]) {
    for (target, source) in target.iter_mut().zip(source) {
        *target = *source;
    }
}

#[cfg(test)]
mod test_chain_signal {
    use super::*;

    /// Fills the signal's channels with constant values.
    fn signal_with(values: &[f32]) -> ChainSignal {
        let mut signal = ChainSignal::new(4);
        signal.reset(values.len());
        for (channel, value) in values.iter().enumerate() {
            signal.channel_mut(channel).expect("channel").fill(*value);
        }
        signal
    }

    #[test]
    fn mono_is_copied_to_both_channels() {
        let mut signal = signal_with(&[0.5]);
        signal.fit_to(2);
        assert_eq!(signal.width, 2);
        assert_eq!(signal.channel(1), Some([0.5; 4].as_slice()));
    }

    #[test]
    fn stereo_is_folded_to_the_average() {
        let mut signal = signal_with(&[0.5, -0.25]);
        signal.fit_to(1);
        assert_eq!(signal.width, 1);
        assert_eq!(signal.channel(0), Some([0.125; 4].as_slice()));
    }

    #[test]
    fn reset_silences_and_limits_the_width() {
        let mut signal = signal_with(&[0.5, 0.5]);
        signal.reset(3);
        assert_eq!(signal.width, 2);
        assert_eq!(signal.channel(0), Some([0.0; 4].as_slice()));
        signal.reset(0);
        assert_eq!(signal.width, 1);
    }
}
