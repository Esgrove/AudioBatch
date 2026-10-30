//! Streaming BS.1770 K weighting and channel-weighted energy for planar audio.
//!
//! The two coefficient stages are combined into one fourth order filter with fixed per-channel history.
//! Energy is accumulated in sample order within each channel, then weighted and summed in channel order.
//! Subnormal history is flushed only at block boundaries without changing the floating point environment.
//! Arithmetic deliberately uses separate multiplication and addition to preserve the reference rounding.
//! The x86 reference enables hardware flush to zero during processing, whereas this kernel leaves that setting alone.
//! Consequently, bit equality with that reference is not guaranteed for arithmetic that underflows within a block.

// Copyright (c) 2011 Jan Kokemüller
// Copyright (c) 2020 Sebastian Dröge <sebastian@centricular.com>
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software
// and associated documentation files (the "Software"),
// to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
// sell copies of the Software, and to permit persons to whom the Software is furnished to do
// so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all copies
// or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
// IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

use std::f64::consts::PI;

/// Largest supported planar channel count, including channels excluded from loudness.
const MAX_CHANNELS: usize = 64;

/// Fixed-storage K weighting with the default BS.1770 channel map and dual-mono treatment of mono audio.
#[derive(Debug, Clone)]
pub(super) struct KWeighting {
    /// Rate-specific coefficients shared by every channel.
    coefficients: Coefficients,
    /// Direct form II history, including the current recurrence value at index zero.
    state: [[f64; 5]; MAX_CHANNELS],
    /// Energy multipliers, with zero marking LFE and other unused channels.
    weights: [f64; MAX_CHANNELS],
    /// Configured number of planes, including unused channels.
    channels: usize,
}

/// Numerator and denominator of the combined fourth order filter.
#[derive(Debug, Clone, Copy)]
struct Coefficients {
    /// Feedforward coefficients in increasing delay order.
    numerator: [f64; 5],
    /// Feedback coefficients in increasing delay order, with a unit leading coefficient.
    denominator: [f64; 5],
}

impl KWeighting {
    /// Creates zeroed filter history for a fixed channel count and sample rate in hertz.
    ///
    /// Four channels use left, right, left surround, and right surround.
    /// Five channels add center before the surrounds.
    /// Other layouts use left, right, center, unused LFE, left surround, and right surround,
    /// with all subsequent channels unused.
    /// A single channel has twice its filtered energy to represent dual mono.
    ///
    /// # Panics
    ///
    /// Panics if the channel count is outside 1 through 64 or the sample rate is zero.
    #[must_use]
    pub(super) fn new(channels: usize, sample_rate: u32) -> Self {
        assert!(
            (1..=MAX_CHANNELS).contains(&channels),
            "K weighting needs 1 through 64 channels"
        );
        assert!(sample_rate > 0, "K weighting needs a positive sample rate");
        let mut weights = [0.0; MAX_CHANNELS];
        for (channel, weight) in weights.iter_mut().take(channels).enumerate() {
            *weight = if channels == 1 {
                2.0
            } else if (channels == 4 && channel >= 2)
                || (channels == 5 && channel >= 3)
                || (channels >= 6 && (4..=5).contains(&channel))
            {
                1.41
            } else if channel < 3 {
                1.0
            } else {
                0.0
            };
        }
        Self {
            coefficients: filter_coefficients(f64::from(sample_rate)),
            state: [[0.0; 5]; MAX_CHANNELS],
            weights,
            channels,
        }
    }

    /// Filters one planar block and returns its weighted sum of squared samples, without averaging.
    ///
    /// History continues across calls, and unused channels are not read or filtered.
    ///
    /// # Panics
    ///
    /// Panics if the number of planes differs from the configured channel count or plane lengths differ.
    #[cfg(test)]
    pub(super) fn energy(&mut self, planes: &[&[f32]]) -> f64 {
        self.energy_split(planes, 0).1
    }

    /// Filters a block and retains channel-weighted energy per frame for an exact partial-window boundary.
    /// The supplied storage is reusable and must have one element per frame.
    #[expect(clippy::suboptimal_flops, reason = "separate operations retain reference rounding")]
    pub(super) fn energy_with_frames(&mut self, planes: &[&[f32]], frames: &mut [f64]) -> f64 {
        assert_eq!(planes.len(), self.channels);
        frames.fill(0.0);
        let mut total = 0.0;
        for ((plane, state), &weight) in planes.iter().zip(&mut self.state).zip(&self.weights) {
            if weight <= 0.0 {
                continue;
            }
            let mut sum = 0.0;
            for (&sample, energy) in plane.iter().zip(frames.iter_mut()) {
                let filtered_energy = filter_energy(std::slice::from_ref(&sample), &self.coefficients, state, 0.0);
                *energy += filtered_energy * weight;
                sum += filtered_energy;
            }
            total += sum * weight;
            for value in state {
                if value.abs() < f64::MIN_POSITIVE {
                    *value = 0.0;
                }
            }
        }
        total
    }

    /// Returns the weighted energy of the prefix `..split` and of the complete block in a single filter pass.
    ///
    /// Neither result is averaged.
    /// The total retains its uninterrupted per-channel summation order regardless of the split,
    /// and history is flushed only after the complete block, not at the prefix boundary.
    ///
    /// # Panics
    ///
    /// Panics if the number of planes differs from the configured channel count,
    /// plane lengths differ, or the split exceeds the frame count.
    #[cfg(test)]
    #[expect(clippy::suboptimal_flops, reason = "separate operations retain reference rounding")]
    pub(super) fn energy_split(&mut self, planes: &[&[f32]], split: usize) -> (f64, f64) {
        assert_eq!(planes.len(), self.channels, "K weighting channel count changed");
        let frames = planes.first().map_or(0, |plane| plane.len());
        assert!(
            planes.iter().all(|plane| plane.len() == frames),
            "K weighting planes have different lengths"
        );
        assert!(split <= frames, "K weighting split exceeds the frame count");

        let mut prefix_energy = 0.0;
        let mut total_energy = 0.0;
        for ((plane, state), &weight) in planes.iter().zip(&mut self.state).zip(&self.weights) {
            if weight <= 0.0 {
                continue;
            }
            let (prefix, remainder) = plane.split_at(split);
            let prefix_sum = filter_energy(prefix, &self.coefficients, state, 0.0);
            let total_sum = filter_energy(remainder, &self.coefficients, state, prefix_sum);
            prefix_energy += prefix_sum * weight;
            total_energy += total_sum * weight;
            for value in state {
                if value.abs() < f64::MIN_POSITIVE {
                    *value = 0.0;
                }
            }
        }
        (prefix_energy, total_energy)
    }
}

/// Filters samples into a sequential energy sum without materializing filtered audio.
#[expect(
    clippy::suboptimal_flops,
    reason = "separate multiplication and addition preserve the reference filter rounding"
)]
fn filter_energy(samples: &[f32], coefficients: &Coefficients, state: &mut [f64; 5], mut sum: f64) -> f64 {
    let numerator = &coefficients.numerator;
    let denominator = &coefficients.denominator;
    for &sample in samples {
        state[0] = f64::from(sample)
            - denominator[1] * state[1]
            - denominator[2] * state[2]
            - denominator[3] * state[3]
            - denominator[4] * state[4];
        let filtered = numerator[0] * state[0]
            + numerator[1] * state[1]
            + numerator[2] * state[2]
            + numerator[3] * state[3]
            + numerator[4] * state[4];
        state[4] = state[3];
        state[3] = state[2];
        state[2] = state[1];
        state[1] = state[0];
        sum += filtered * filtered;
    }
    sum
}

/// Combines the shelving and high-pass stages while retaining the coefficient evaluation order.
#[expect(
    clippy::suboptimal_flops,
    reason = "separate multiplication and addition preserve the reference coefficient rounding"
)]
fn filter_coefficients(rate: f64) -> Coefficients {
    let (shelf_numerator, shelf_denominator) = shelving_coefficients(rate);
    let (high_pass_numerator, high_pass_denominator) = high_pass_coefficients(rate);
    Coefficients {
        numerator: [
            shelf_numerator[0] * high_pass_numerator[0],
            shelf_numerator[0] * high_pass_numerator[1] + shelf_numerator[1] * high_pass_numerator[0],
            shelf_numerator[0] * high_pass_numerator[2]
                + shelf_numerator[1] * high_pass_numerator[1]
                + shelf_numerator[2] * high_pass_numerator[0],
            shelf_numerator[1] * high_pass_numerator[2] + shelf_numerator[2] * high_pass_numerator[1],
            shelf_numerator[2] * high_pass_numerator[2],
        ],
        denominator: [
            shelf_denominator[0] * high_pass_denominator[0],
            shelf_denominator[0] * high_pass_denominator[1] + shelf_denominator[1] * high_pass_denominator[0],
            shelf_denominator[0] * high_pass_denominator[2]
                + shelf_denominator[1] * high_pass_denominator[1]
                + shelf_denominator[2] * high_pass_denominator[0],
            shelf_denominator[1] * high_pass_denominator[2] + shelf_denominator[2] * high_pass_denominator[1],
            shelf_denominator[2] * high_pass_denominator[2],
        ],
    }
}

/// Calculates the BS.1770 prefilter's shelving biquad numerator and denominator.
#[expect(
    clippy::suboptimal_flops,
    reason = "separate multiplication and addition preserve the reference coefficient rounding"
)]
fn shelving_coefficients(rate: f64) -> ([f64; 3], [f64; 3]) {
    let frequency = 1_681.974_450_955_533;
    let gain = 3.999_843_853_973_347;
    let quality = 0.707_175_236_955_419_6;
    let tangent = f64::tan(PI * frequency / rate);
    let high_gain = f64::powf(10.0, gain / 20.0);
    let band_gain = f64::powf(high_gain, 0.499_666_774_154_541_6);
    let normalization = 1.0 + tangent / quality + tangent * tangent;
    (
        [
            (high_gain + band_gain * tangent / quality + tangent * tangent) / normalization,
            2.0 * (tangent * tangent - high_gain) / normalization,
            (high_gain - band_gain * tangent / quality + tangent * tangent) / normalization,
        ],
        [
            1.0,
            2.0 * (tangent * tangent - 1.0) / normalization,
            (1.0 - tangent / quality + tangent * tangent) / normalization,
        ],
    )
}

/// Calculates the BS.1770 RLB high-pass biquad numerator and denominator.
#[expect(
    clippy::suboptimal_flops,
    reason = "separate multiplication and addition preserve the reference coefficient rounding"
)]
fn high_pass_coefficients(rate: f64) -> ([f64; 3], [f64; 3]) {
    let frequency = 38.135_470_876_024_44;
    let quality = 0.500_327_037_323_877_3;
    let tangent = f64::tan(PI * frequency / rate);
    (
        [1.0, -2.0, 1.0],
        [
            1.0,
            2.0 * (tangent * tangent - 1.0) / (1.0 + tangent / quality + tangent * tangent),
            (1.0 - tangent / quality + tangent * tangent) / (1.0 + tangent / quality + tangent * tangent),
        ],
    )
}

#[cfg(test)]
mod test_coefficients {
    use super::{filter_coefficients, high_pass_coefficients, shelving_coefficients};

    /// Checks each coefficient against the 48 kHz BS.1770-5 table value.
    fn assert_coefficients(actual: &[f64], expected: &[f64]) {
        for (&actual, &expected) in actual.iter().zip(expected) {
            assert!((actual - expected).abs() < 1.0e-13, "{actual} differs from {expected}");
        }
    }

    /// The shelving stage matches BS.1770-5 table 1.
    #[test]
    fn shelf_matches_standard_at_48_khz() {
        let (numerator, denominator) = shelving_coefficients(48_000.0);
        assert_coefficients(
            &numerator,
            &[1.535_124_859_586_97, -2.691_696_189_406_38, 1.198_392_810_852_85],
        );
        assert_coefficients(&denominator, &[1.0, -1.690_659_293_182_41, 0.732_480_774_215_85]);
    }

    /// The RLB stage matches BS.1770-5 table 2 without normalizing its numerator.
    #[test]
    fn high_pass_matches_standard_at_48_khz() {
        let (numerator, denominator) = high_pass_coefficients(48_000.0);
        assert_coefficients(&numerator, &[1.0, -2.0, 1.0]);
        assert_coefficients(&denominator, &[1.0, -1.990_047_454_833_98, 0.990_072_250_366_21]);
    }

    /// Combining the stages is polynomial convolution, with no coefficient renormalization.
    #[test]
    fn combined_coefficients_match_convolution() {
        let (shelf_numerator, shelf_denominator) = shelving_coefficients(48_000.0);
        let (high_pass_numerator, high_pass_denominator) = high_pass_coefficients(48_000.0);
        let combined = filter_coefficients(48_000.0);
        for ((first, second), actual) in [
            ((shelf_numerator, high_pass_numerator), combined.numerator),
            ((shelf_denominator, high_pass_denominator), combined.denominator),
        ] {
            let mut expected = [0.0; 5];
            for (delay, expected) in expected.iter_mut().enumerate() {
                for (first_delay, &first_value) in first.iter().enumerate() {
                    for (second_delay, &second_value) in second.iter().enumerate() {
                        if first_delay + second_delay == delay {
                            *expected = f64::mul_add(first_value, second_value, *expected);
                        }
                    }
                }
            }
            assert_coefficients(&actual, &expected);
        }
    }
}

#[cfg(test)]
mod test_filter {
    use super::{Coefficients, KWeighting, filter_coefficients};

    /// Generates scalar reference output with four explicit delay variables.
    #[expect(
        clippy::suboptimal_flops,
        reason = "the reference deliberately uses separate multiplication and addition"
    )]
    fn scalar_reference(samples: &[f32], coefficients: Coefficients) -> Vec<f64> {
        let [_, first_feedback, second_feedback, third_feedback, fourth_feedback] = coefficients.denominator;
        let [current_gain, first_gain, second_gain, third_gain, fourth_gain] = coefficients.numerator;
        let mut first_delay = 0.0;
        let mut second_delay = 0.0;
        let mut third_delay = 0.0;
        let mut fourth_delay = 0.0;
        samples
            .iter()
            .map(|&sample| {
                let current = f64::from(sample)
                    - first_feedback * first_delay
                    - second_feedback * second_delay
                    - third_feedback * third_delay
                    - fourth_feedback * fourth_delay;
                let output = current_gain * current
                    + first_gain * first_delay
                    + second_gain * second_delay
                    + third_gain * third_delay
                    + fourth_gain * fourth_delay;
                fourth_delay = third_delay;
                third_delay = second_delay;
                second_delay = first_delay;
                first_delay = current;
                output
            })
            .collect()
    }

    /// Every impulse response sample retains the scalar fourth order recurrence's exact arithmetic.
    #[test]
    fn impulse_matches_scalar_reference() {
        let mut impulse = [0.0; 512];
        impulse[0] = 1.0;
        for sample_rate in [8000, 44_100, 48_000, 192_000] {
            let expected = scalar_reference(&impulse, filter_coefficients(f64::from(sample_rate)));
            let mut filter = KWeighting::new(2, sample_rate);
            for (&sample, expected) in impulse.iter().zip(expected) {
                let actual = filter.energy(&[&[sample], &[0.0]]);
                assert_eq!(actual.to_bits(), (expected * expected).to_bits());
            }
        }
    }

    /// Prefix reporting changes neither the full-block sum nor the resulting filter history.
    #[test]
    fn split_preserves_total_and_reports_exact_prefix() {
        let samples: Vec<_> = (0..257)
            .map(|frame| ((f64::from(frame) * 0.17).sin() * 0.5) as f32)
            .collect();
        let other: Vec<_> = samples.iter().rev().copied().collect();
        for split in [0, 1, 73, 256, 257] {
            let mut whole_filter = KWeighting::new(2, 44_100);
            let whole = whole_filter.energy(&[&samples, &other]);
            let mut split_filter = KWeighting::new(2, 44_100);
            let (prefix, total) = split_filter.energy_split(&[&samples, &other], split);
            let mut prefix_filter = KWeighting::new(2, 44_100);
            let expected_prefix = prefix_filter.energy(&[samples.split_at(split).0, other.split_at(split).0]);
            assert_eq!(prefix.to_bits(), expected_prefix.to_bits());
            assert_eq!(total.to_bits(), whole.to_bits());
            for (actual, expected) in split_filter
                .state
                .iter()
                .flatten()
                .zip(whole_filter.state.iter().flatten())
            {
                assert_eq!(actual.to_bits(), expected.to_bits());
            }
        }
    }

    /// Irregular block lengths preserve filter history and return sums rather than averages.
    #[test]
    #[expect(
        clippy::suboptimal_flops,
        reason = "test sequential reference summation without fused operations"
    )]
    fn streaming_blocks_match_scalar_energy() {
        let mut samples = [0.0; 1024];
        samples[0] = 1.0;
        samples[421] = -0.5;
        let expected = scalar_reference(&samples, filter_coefficients(48_000.0));
        let mut filter = KWeighting::new(1, 48_000);
        for (samples, expected) in samples.chunks(73).zip(expected.chunks(73)) {
            let mut sum = 0.0;
            for &sample in expected {
                sum += sample * sample;
            }
            assert_eq!(filter.energy(&[samples]).to_bits(), (sum * 2.0).to_bits());
        }
    }

    /// A nonempty block flushes all subnormal recurrence state after filtering its final sample.
    #[test]
    fn processed_subnormal_history_is_flushed() {
        let mut filter = KWeighting::new(1, 48_000);
        filter.state[0] = [f64::from_bits(1); 5];
        let (prefix, total) = filter.energy_split(&[&[0.0, 0.0]], 1);
        assert!(prefix.abs() < f64::MIN_POSITIVE);
        assert!(total.abs() < f64::MIN_POSITIVE);
        assert!(filter.state[0].iter().all(|value| value.to_bits() == 0));
    }

    /// An empty block flushes subnormal history, but keeps normal state and does not change the environment.
    #[test]
    fn subnormal_history_is_flushed_at_block_boundary() {
        let mut filter = KWeighting::new(1, 48_000);
        filter.state[0] = [
            f64::from_bits(1),
            -f64::from_bits(1),
            f64::MIN_POSITIVE,
            -f64::MIN_POSITIVE,
            0.0,
        ];
        assert!(filter.energy(&[&[]]).abs() < f64::MIN_POSITIVE);
        let expected = [0.0_f64, 0.0, f64::MIN_POSITIVE, -f64::MIN_POSITIVE, 0.0];
        for (actual, expected) in filter.state[0].iter().zip(expected) {
            assert_eq!(actual.to_bits(), expected.to_bits());
        }
    }
}

#[cfg(test)]
mod test_channels {
    use super::{KWeighting, MAX_CHANNELS};

    /// Default maps include the four- and five-channel exceptions and ignore channels after the sixth.
    #[test]
    fn default_channel_weights_match_layouts() {
        let layouts: &[(usize, &[f64])] = &[
            (1, &[2.0]),
            (2, &[1.0, 1.0]),
            (3, &[1.0, 1.0, 1.0]),
            (4, &[1.0, 1.0, 1.41, 1.41]),
            (5, &[1.0, 1.0, 1.0, 1.41, 1.41]),
            (6, &[1.0, 1.0, 1.0, 0.0, 1.41, 1.41]),
            (8, &[1.0, 1.0, 1.0, 0.0, 1.41, 1.41, 0.0, 0.0]),
        ];
        for &(channels, expected) in layouts {
            let filter = KWeighting::new(channels, 48_000);
            for (&actual, &expected) in filter.weights.iter().zip(expected) {
                assert!((actual - expected).abs() < f64::EPSILON);
            }
            assert!(
                filter
                    .weights
                    .iter()
                    .skip(channels)
                    .all(|weight| weight.abs() < f64::MIN_POSITIVE)
            );
        }
        let filter = KWeighting::new(MAX_CHANNELS, 48_000);
        assert!(
            filter
                .weights
                .iter()
                .skip(6)
                .all(|weight| weight.abs() < f64::MIN_POSITIVE)
        );
    }

    /// Mono dual-mono energy equals stereo energy when both channels carry the same samples.
    #[test]
    fn mono_matches_duplicated_stereo() {
        let samples = [1.0, 0.5, -0.25, 0.0, -1.0];
        let mono = KWeighting::new(1, 48_000).energy(&[&samples]);
        let stereo = KWeighting::new(2, 48_000).energy(&[&samples, &samples]);
        assert_eq!(mono.to_bits(), stereo.to_bits());
    }

    /// Each surround channel's multiplier is applied to its complete sequential energy sum.
    #[test]
    fn surround_energy_uses_default_weights() {
        let samples = [1.0, -0.5, 0.25, 0.0];
        let silence = [0.0; 4];
        let base = KWeighting::new(2, 48_000).energy(&[&samples, &silence]);
        for channels in [4, 5, 6, 8] {
            for channel in 0..channels {
                let mut filter = KWeighting::new(channels, 48_000);
                let mut planes = [&silence[..]; MAX_CHANNELS];
                *planes.get_mut(channel).expect("channel fits fixed storage") = &samples;
                let weight = *filter.weights.get(channel).expect("channel fits fixed storage");
                let actual = filter.energy(planes.split_at(channels).0);
                assert_eq!(actual.to_bits(), (base * weight).to_bits());
            }
        }
    }

    /// LFE and unused channels contribute no energy and leave their filter state untouched, even for NaN input.
    #[test]
    fn silent_lfe_and_unused_channels_are_ignored() {
        let silence = [0.0; 8];
        let ignored = [f32::NAN; 8];
        let mut filter = KWeighting::new(8, 48_000);
        let (prefix, total) = filter.energy_split(
            &[
                &silence, &silence, &silence, &ignored, &silence, &silence, &ignored, &ignored,
            ],
            3,
        );
        assert!(prefix.abs() < f64::MIN_POSITIVE);
        assert!(total.abs() < f64::MIN_POSITIVE);
        assert!(
            filter
                .state
                .iter()
                .flatten()
                .all(|value| value.abs() < f64::MIN_POSITIVE)
        );
    }
}
