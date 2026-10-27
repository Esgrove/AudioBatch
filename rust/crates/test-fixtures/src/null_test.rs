//! The null test that subtracts an expected output from an actual one, in 16-bit steps.
//!
//! The processed outputs are 16-bit files, so their samples are exact multiples of one step,
//! and comparing them as integer steps finds any difference, down to a single step.
//! When the two do not cancel,
//! the cross-correlation of their first channels tells whether the output is only shifted in time,
//! which points at a latency problem rather than a level one.
//! The C++ tests have the same helper in `cpp/tests/TestHelpers.cpp`.

use std::fmt;

/// Scale from a float sample to a 16-bit sample value.
const PCM16_SCALE: f32 = 32_768.0;

/// Largest lag in either direction that the cross-correlation checks, in samples.
const MAXIMUM_LAG: isize = 256;

/// Result of subtracting an expected signal from an actual one.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct NullTestResult {
    /// Channels of the expected signal.
    pub expected_channels: usize,
    /// Channels of the actual signal.
    pub actual_channels: usize,
    /// Frames of the expected signal.
    pub expected_frames: usize,
    /// Frames of the actual signal.
    pub actual_frames: usize,
    /// Samples over all channels that differ within the common length.
    pub differing_samples: usize,
    /// Largest absolute difference within the common length, in 16-bit steps.
    pub peak_difference: i32,
    /// Offset of the actual first channel against the expected one, in samples, where the two correlate best.
    /// Only computed when the signals do not cancel, and zero otherwise.
    pub best_lag: isize,
}

impl NullTestResult {
    /// Returns true when both signals have the same layout and length and every sample cancels out.
    #[must_use]
    pub const fn cancels(&self) -> bool {
        self.expected_channels == self.actual_channels
            && self.expected_frames == self.actual_frames
            && self.differing_samples == 0
    }
}

impl fmt::Display for NullTestResult {
    /// Describes the layouts and lengths, the differing sample count, the residual peak in dBFS, and the best lag.
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        let residual = if self.peak_difference > 0 {
            #[expect(clippy::cast_precision_loss, reason = "a 16-bit difference is exact in f32")]
            let peak = self.peak_difference as f32 / PCM16_SCALE;
            format!("{:.1} dBFS", 20.0 * peak.log10())
        } else {
            String::from("silent")
        };
        write!(
            formatter,
            "{} channels and {} frames expected, {} channels and {} frames written, {} samples differ, \
             residual peak {residual} ({} steps), best correlation at a lag of {} samples",
            self.expected_channels,
            self.expected_frames,
            self.actual_channels,
            self.actual_frames,
            self.differing_samples,
            self.peak_difference,
            self.best_lag
        )
    }
}

/// Subtracts the expected signal from the actual one, both given as float samples per channel,
/// and finds the lag at which their first channels correlate best when they do not cancel.
#[must_use]
pub fn null_test(expected: &[Vec<f32>], actual: &[Vec<f32>]) -> NullTestResult {
    let expected_steps = to_steps(expected);
    let actual_steps = to_steps(actual);
    let mut result = NullTestResult {
        expected_channels: expected.len(),
        actual_channels: actual.len(),
        expected_frames: expected.first().map_or(0, Vec::len),
        actual_frames: actual.first().map_or(0, Vec::len),
        differing_samples: 0,
        peak_difference: 0,
        best_lag: 0,
    };
    for (expected_channel, actual_channel) in expected_steps.iter().zip(&actual_steps) {
        for (expected_sample, actual_sample) in expected_channel.iter().zip(actual_channel) {
            let difference = (actual_sample - expected_sample).abs();
            if difference != 0 {
                result.differing_samples += 1;
                result.peak_difference = result.peak_difference.max(difference);
            }
        }
    }
    if !result.cancels()
        && let (Some(expected_channel), Some(actual_channel)) = (expected_steps.first(), actual_steps.first())
    {
        result.best_lag = best_lag(expected_channel, actual_channel);
    }
    result
}

/// Converts float samples to 16-bit steps.
fn to_steps(channels: &[Vec<f32>]) -> Vec<Vec<i32>> {
    channels
        .iter()
        .map(|channel| {
            channel
                .iter()
                .map(|sample| {
                    #[expect(
                        clippy::cast_possible_truncation,
                        reason = "a sample of a 16-bit file is a whole number of steps"
                    )]
                    let steps = (sample * PCM16_SCALE).round() as i32;
                    steps
                })
                .collect()
        })
        .collect()
}

/// Returns the lag of the actual signal against the expected one, within the checked range,
/// at which their cross-correlation is largest.
fn best_lag(expected: &[i32], actual: &[i32]) -> isize {
    let mut best = (0, i64::MIN);
    for lag in -MAXIMUM_LAG..=MAXIMUM_LAG {
        let correlation: i64 = expected
            .iter()
            .enumerate()
            .filter_map(|(index, expected_sample)| {
                let actual_index = index.checked_add_signed(lag)?;
                let actual_sample = actual.get(actual_index)?;
                Some(i64::from(*expected_sample) * i64::from(*actual_sample))
            })
            .sum();
        if correlation > best.1 {
            best = (lag, correlation);
        }
    }
    best.0
}

#[cfg(test)]
mod test_null_test {
    use super::*;

    /// Returns a stereo test signal of the given length, with a different tone on each channel.
    fn signal(frames: usize) -> Vec<Vec<f32>> {
        let tone = |period: f32| {
            (0..frames)
                .map(|frame| {
                    #[expect(clippy::cast_precision_loss, reason = "the frame index is small")]
                    let phase = frame as f32 / period;
                    ((phase * std::f32::consts::TAU).sin() * 16_000.0).round() / PCM16_SCALE
                })
                .collect()
        };
        vec![tone(37.0), tone(91.0)]
    }

    #[test]
    fn identical_signals_cancel() {
        let audio = signal(2_000);
        let result = null_test(&audio, &audio);
        assert!(result.cancels(), "{result}");
        assert_eq!(result.best_lag, 0);
    }

    #[test]
    fn a_single_step_is_reported_with_its_level() {
        let expected = signal(2_000);
        let mut actual = expected.clone();
        actual[1][500] += 1.0 / PCM16_SCALE;
        let result = null_test(&expected, &actual);
        assert!(!result.cancels());
        assert_eq!(result.differing_samples, 1);
        assert_eq!(result.peak_difference, 1);
        assert_eq!(result.best_lag, 0);
        assert!(result.to_string().contains("-90.3 dBFS"), "{result}");
    }

    #[test]
    fn a_delayed_signal_reports_its_lag() {
        let expected = signal(2_000);
        let delayed: Vec<Vec<f32>> = expected
            .iter()
            .map(|channel| {
                let mut shifted = vec![0.0; 12];
                shifted.extend_from_slice(&channel[..channel.len() - 12]);
                shifted
            })
            .collect();
        let result = null_test(&expected, &delayed);
        assert!(!result.cancels());
        assert_eq!(result.best_lag, 12);
    }

    #[test]
    fn different_layouts_and_lengths_do_not_cancel() {
        let stereo = signal(1_000);
        let mono = vec![stereo[0].clone()];
        assert!(!null_test(&stereo, &mono).cancels());
        let shorter: Vec<Vec<f32>> = stereo.iter().map(|channel| channel[..999].to_vec()).collect();
        let result = null_test(&stereo, &shorter);
        assert!(!result.cancels());
        assert_eq!(result.differing_samples, 0);
    }
}
