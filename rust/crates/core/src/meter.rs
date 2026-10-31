//! The block meter that turns decoded planar audio into the measurements of one file.
//!
//! `BlockMeter` regroups the decoded chunks into fixed blocks of one tenth of a second,
//! one block per short-term loudness update as EBU Tech 3341 asks for,
//! and feeds each block to the loudness meter, the per-channel sample extremes, the waveform, and overload history.
//! A mono source is measured as dual mono, so it reads the same loudness as the same content in stereo.
//! The meter takes samples from memory, so the tests and benchmarks measure signals without decoding a file.
//! It is public only for the benchmarks, which is why it is hidden from the documentation.

use std::sync::Arc;

use crate::loudness::gating::LoudnessMeter;

use crate::loudness::true_peak::TruePeak;
use crate::overload::{OverloadHistory, OverloadHistoryBuilder, SAMPLE_OVER, TRUE_PEAK_OVER};
use crate::record::{NEGATIVE_INFINITY_LOUDNESS, ShortTermLoudnessHistory, normalize_loudness};
use crate::waveform::{WaveformBuilder, WaveformOverview};

/// A loudness measurement failure.
#[doc(hidden)]
#[derive(Debug, thiserror::Error)]
pub enum MeterError {
    /// The stream is outside the supported one to 64 channels or 16 to 2822400 Hz range.
    #[error("Could not initialize loudness analyzer")]
    UnsupportedStream,
}

/// Accumulates decoded audio into fixed-size analysis blocks and measures each completed block.
#[doc(hidden)]
pub struct BlockMeter {
    meter: LoudnessMeter,
    true_peak: Option<TruePeak>,
    sample_rate: u32,
    block_size: usize,
    block: Vec<Vec<f32>>,
    min_samples: [f32; 64],
    max_samples: [f32; 64],
    max_short_term_lufs: f64,
    history: Vec<f32>,
    waveform: WaveformBuilder,
    overloads: OverloadHistoryBuilder,
}

/// Everything the meter measured over a whole stream.
#[doc(hidden)]
#[derive(Debug, Clone)]
pub struct MeterOutput {
    /// Smallest sample of each channel, zero or below.
    pub min_samples: Vec<f32>,
    /// Largest sample of each channel, zero or above.
    pub max_samples: Vec<f32>,
    /// Sample extreme of each channel with its original sign, preferring the positive extreme on a tie.
    pub signed_peaks: Vec<f32>,
    /// True peak of each channel as a linear amplitude.
    pub true_peaks: Vec<f64>,
    /// Gated integrated loudness in LUFS, or the silence sentinel.
    pub integrated_lufs: f64,
    /// Largest short-term loudness in LUFS, or the silence sentinel.
    pub max_short_term_lufs: f64,
    /// Short-term loudness after each block, in LUFS.
    pub history: Vec<f32>,
    /// Frames per block, and so per history value.
    pub samples_per_point: u32,
    /// Waveform overview of the stream.
    pub waveform: Arc<WaveformOverview>,
    /// Bounded source overload flags measured at the same fixed block boundaries.
    pub overloads: Arc<OverloadHistory>,
}

impl BlockMeter {
    /// Creates a meter for the stream, with integrated, short-term, and true peak measurement.
    ///
    /// # Errors
    ///
    /// Returns an error when the meter does not support the channel count or the sample rate.
    pub fn new(channels: u32, sample_rate: u32, expected_frames: Option<u64>) -> Result<Self, MeterError> {
        if !(1..=64).contains(&channels) || !(16..=2_822_400).contains(&sample_rate) {
            return Err(MeterError::UnsupportedStream);
        }
        let channel_count = channels as usize;
        let meter = LoudnessMeter::new(channel_count, sample_rate, expected_frames);
        let block_size = ShortTermLoudnessHistory::samples_per_point_for_rate(sample_rate) as usize;
        Ok(Self {
            meter,
            true_peak: (sample_rate < 192_000).then(|| TruePeak::new(channel_count, sample_rate)),
            sample_rate,
            block_size,
            block: vec![Vec::with_capacity(block_size); channel_count],
            min_samples: [0.0; 64],
            max_samples: [0.0; 64],
            max_short_term_lufs: NEGATIVE_INFINITY_LOUDNESS,
            history: Vec::with_capacity(
                expected_frames
                    .and_then(|frames| usize::try_from(frames.div_ceil(block_size as u64)).ok())
                    .unwrap_or(0),
            ),
            waveform: WaveformBuilder::new(channel_count, expected_frames),
            overloads: OverloadHistoryBuilder::new(channels, sample_rate),
        })
    }

    /// Appends decoded planar samples, one slice per channel, measuring every block that fills up.
    /// A channel shorter than the first one, including an omitted plane, is padded with silence.
    /// Complete blocks borrow the decoder's planes directly, while partial blocks retain reusable sample storage.
    pub fn push<P: AsRef<[f32]>>(&mut self, chunk: &[P]) {
        let frame_count = chunk.first().map_or(0, |plane| plane.as_ref().len());
        let mut offset = 0;
        while offset < frame_count {
            let filled = self.block.first().map_or(0, Vec::len);
            let take = (self.block_size - filled).min(frame_count - offset);
            if filled == 0 && take == self.block_size {
                let mut planes = [&[][..]; 64];
                let channels = self.block.len();
                let mut complete = true;
                for (channel, plane) in planes.iter_mut().take(channels).enumerate() {
                    if let Some(segment) = chunk
                        .get(channel)
                        .and_then(|samples| samples.as_ref().get(offset..offset + take))
                    {
                        *plane = segment;
                    } else {
                        complete = false;
                        break;
                    }
                }
                if complete {
                    self.measure_planes(
                        planes
                            .get(..channels)
                            .expect("validated channel count fits fixed storage"),
                    );
                    offset += take;
                    continue;
                }
            }
            for (channel, block_channel) in self.block.iter_mut().enumerate() {
                let samples = chunk.get(channel).map_or(&[][..], AsRef::as_ref);
                let available = samples.len().saturating_sub(offset).min(take);
                if let Some(segment) = samples.get(offset..offset + available) {
                    block_channel.extend_from_slice(segment);
                }
                block_channel.resize(block_channel.len() + take - available, 0.0);
            }
            offset += take;
            if filled + take == self.block_size {
                self.measure_block();
            }
        }
    }

    /// Measures a final partial block and returns the measurements of the stream of `length_in_samples` frames.
    #[must_use]
    pub fn finish(mut self, length_in_samples: u64) -> MeterOutput {
        if self.block.first().is_some_and(|samples| !samples.is_empty()) {
            self.measure_block();
        }
        let channels = self.block.len();
        let min_samples = self
            .min_samples
            .get(..channels)
            .expect("validated channel count")
            .to_vec();
        let max_samples = self
            .max_samples
            .get(..channels)
            .expect("validated channel count")
            .to_vec();
        let signed_peaks = min_samples
            .iter()
            .zip(&max_samples)
            .map(|(&minimum, &maximum)| crate::analysis::signed_peak_from_extrema(minimum, maximum))
            .collect();
        let true_peaks = self.true_peak.as_ref().map_or_else(
            || {
                // No interpolation is needed at these rates, reuse the signed sample extremes.
                min_samples
                    .iter()
                    .zip(&max_samples)
                    .map(|(&minimum, &maximum)| f64::from(minimum).abs().max(f64::from(maximum).abs()))
                    .collect()
            },
            |meter| meter.peaks().to_vec(),
        );
        let integrated_lufs = normalize_loudness(self.meter.integrated());
        MeterOutput {
            min_samples,
            max_samples,
            signed_peaks,
            true_peaks,
            integrated_lufs,
            max_short_term_lufs: self.max_short_term_lufs,
            history: self.history,
            samples_per_point: self.block_size as u32,
            waveform: self.waveform.finish(self.sample_rate, length_in_samples),
            overloads: Arc::new(self.overloads.finish(length_in_samples)),
        }
    }

    /// Feeds a buffered block without allocating plane references and retains its sample storage for reuse.
    fn measure_block(&mut self) {
        let mut block = std::mem::take(&mut self.block);
        let mut planes = [&[][..]; 64];
        for (plane, samples) in planes.iter_mut().zip(&block) {
            *plane = samples;
        }
        self.measure_planes(
            planes
                .get(..block.len())
                .expect("validated channel count fits fixed storage"),
        );
        for samples in &mut block {
            samples.clear();
        }
        self.block = block;
    }

    /// Measures one fixed analysis block, whether borrowed directly or assembled from decoder packets.
    fn measure_planes(&mut self, planes: &[&[f32]]) {
        let mut flags = [0_u8; 64];
        let mut block_minima = [0.0_f32; 64];
        let mut block_maxima = [0.0_f32; 64];
        for (channel, ((samples, minimum), maximum)) in planes
            .iter()
            .zip(&mut self.min_samples)
            .zip(&mut self.max_samples)
            .enumerate()
        {
            let (mut block_minimum, mut block_maximum) = (0.0_f32, 0.0_f32);
            for &sample in *samples {
                if sample < block_minimum {
                    block_minimum = sample;
                }
                if sample > block_maximum {
                    block_maximum = sample;
                }
            }
            *block_minima.get_mut(channel).expect("validated channel") = block_minimum;
            *block_maxima.get_mut(channel).expect("validated channel") = block_maximum;
            if block_minimum < *minimum {
                *minimum = block_minimum;
            }
            if block_maximum > *maximum {
                *maximum = block_maximum;
            }
            if (block_minimum < -1.0 || block_maximum > 1.0)
                && let Some(flag) = flags.get_mut(channel)
            {
                *flag = SAMPLE_OVER;
                if self.true_peak.is_none() {
                    *flag |= TRUE_PEAK_OVER;
                }
            }
        }
        self.waveform.push_extrema(planes, &block_minima, &block_maxima);
        if let Some(true_peak) = &mut self.true_peak {
            true_peak.push(planes);
            for (flag, &peak) in flags.iter_mut().zip(true_peak.previous_peaks()) {
                if peak > 1.0 {
                    *flag |= TRUE_PEAK_OVER;
                }
            }
        }
        let short_term = normalize_loudness(self.meter.push(planes));
        self.max_short_term_lufs = self.max_short_term_lufs.max(short_term);
        self.history.push(short_term as f32);
        self.overloads.push(
            flags
                .get(..planes.len())
                .expect("validated channel count fits fixed storage"),
        );
    }
}

#[cfg(test)]
mod test_chunk_size_invariance {
    use crate::golden::{GoldenText, measure_signal};
    use crate::signals::{SignalKind, Xorshift, generate};

    /// Returns every measured value as text with its exact bits, so two outputs compare bit for bit.
    fn exact(output: &super::MeterOutput) -> String {
        let mut text = GoldenText::default();
        text.meter(output);
        text.blob(
            "overloads.blob",
            &output.overloads.to_bytes().expect("valid overload history"),
        );
        text.text().to_string()
    }

    /// Measures the signal in a whole buffer and in every listed chunk size, and requires identical results.
    fn assert_invariant(kind: SignalKind, sample_rate: u32, channels: usize, seconds: f64) {
        let frames = (f64::from(sample_rate) * seconds) as usize;
        let planes = generate(kind, sample_rate, channels, frames);
        let block = super::ShortTermLoudnessHistory::samples_per_point_for_rate(sample_rate) as usize;
        let reference = exact(&measure_signal(&planes, sample_rate, frames));
        for chunk_frames in [1, 7, 64, 4096, block - 1, block, block + 1] {
            let output = exact(&measure_signal(&planes, sample_rate, chunk_frames));
            assert_eq!(
                output, reference,
                "{kind:?} at {sample_rate} Hz in chunks of {chunk_frames}"
            );
        }
    }

    #[test]
    fn stereo_music_at_44100() {
        assert_invariant(SignalKind::MusicLike, 44_100, 2, 0.75);
    }

    #[test]
    fn mono_noise_at_48000() {
        assert_invariant(SignalKind::Noise, 48_000, 1, 0.75);
    }

    #[test]
    fn surround_intersample_at_96000() {
        assert_invariant(SignalKind::Intersample, 96_000, 6, 0.5);
    }

    #[test]
    fn stereo_fade_at_192000() {
        assert_invariant(SignalKind::FadeToSubnormal, 192_000, 2, 0.5);
    }

    #[test]
    fn random_chunk_sizes() {
        let sample_rate = 44_100;
        let planes = generate(SignalKind::MusicLike, sample_rate, 2, 40_000);
        let reference = exact(&measure_signal(&planes, sample_rate, 40_000));
        let mut random = Xorshift::new(7);
        let mut meter = super::BlockMeter::new(2, sample_rate, Some(40_000)).expect("meter");
        let mut offset = 0;
        while offset < 40_000 {
            let end = (offset + 1 + random.next_below(9_000) as usize).min(40_000);
            let chunk: Vec<&[f32]> = planes.iter().map(|plane| &plane[offset..end]).collect();
            meter.push(&chunk);
            offset = end;
        }
        assert_eq!(exact(&meter.finish(40_000)), reference);
    }
}

#[cfg(test)]
mod test_channel_handling {
    use super::BlockMeter;
    use crate::golden::measure_signal;
    use crate::signals::{SignalKind, generate};

    #[test]
    fn mono_reads_like_the_same_content_on_both_stereo_channels() {
        let mono = generate(SignalKind::MusicLike, 48_000, 1, 48_000);
        let stereo = vec![mono[0].clone(), mono[0].clone()];
        let mono_output = measure_signal(&mono, 48_000, 1_000);
        let stereo_output = measure_signal(&stereo, 48_000, 1_000);
        assert_eq!(
            mono_output.integrated_lufs.to_bits(),
            stereo_output.integrated_lufs.to_bits()
        );
        assert_eq!(mono_output.history, stereo_output.history);
        assert_eq!(
            mono_output.true_peaks[0].to_bits(),
            stereo_output.true_peaks[1].to_bits()
        );
    }

    #[test]
    fn identical_channels_measure_identical_peaks() {
        let mono = generate(SignalKind::Intersample, 44_100, 1, 20_000);
        let stereo = vec![mono[0].clone(), mono[0].clone()];
        let output = measure_signal(&stereo, 44_100, 1_000);
        assert_eq!(output.true_peaks[0].to_bits(), output.true_peaks[1].to_bits());
        assert_eq!(output.min_samples[0].to_bits(), output.min_samples[1].to_bits());
        assert_eq!(output.max_samples[0].to_bits(), output.max_samples[1].to_bits());
    }

    #[test]
    fn a_short_channel_is_padded_with_silence() {
        let left = generate(SignalKind::Sine, 44_100, 1, 10_000).remove(0);
        let mut padded = BlockMeter::new(2, 44_100, None).expect("meter");
        padded.push(&[left.as_slice(), &[]]);
        let padded = padded.finish(10_000);
        let silent = measure_signal(&[left, vec![0.0; 10_000]], 44_100, 10_000);
        assert_eq!(padded.true_peaks, silent.true_peaks);
        assert_eq!(padded.history, silent.history);
        assert_eq!(padded.max_samples, silent.max_samples);
        assert_eq!(padded.waveform, silent.waveform);
    }

    #[test]
    fn a_short_channel_retains_its_nonempty_prefix() {
        let left = vec![0.25; 10_000];
        let right = vec![-0.75; 5_123];
        let mut meter = BlockMeter::new(2, 44_100, None).expect("meter");
        meter.push(&[left.as_slice(), right.as_slice()]);
        let output = meter.finish(10_000);
        let mut padded = right;
        padded.resize(left.len(), 0.0);
        let expected = measure_signal(&[left, padded], 44_100, 1_000);
        assert_eq!(output.min_samples, expected.min_samples);
        assert_eq!(output.true_peaks, expected.true_peaks);
        assert_eq!(output.history, expected.history);
        assert_eq!(output.waveform, expected.waveform);
    }

    #[test]
    fn missing_planes_are_silent_even_after_a_partial_block() {
        let left = vec![0.25; 5_000];
        let mut meter = BlockMeter::new(3, 44_100, None).expect("meter");
        meter.push(&[&left[..7]]);
        meter.push(&[&left[7..]]);
        let output = meter.finish(5_000);
        let expected = measure_signal(&[left, vec![0.0; 5_000], vec![0.0; 5_000]], 44_100, 1_000);
        assert_eq!(output.true_peaks, expected.true_peaks);
        assert_eq!(output.history, expected.history);
        assert_eq!(output.waveform, expected.waveform);
    }

    #[test]
    fn unsupported_streams_fail_to_initialize() {
        assert!(BlockMeter::new(0, 44_100, None).is_err());
        assert!(BlockMeter::new(2, 0, None).is_err());
        assert!(BlockMeter::new(65, 44_100, None).is_err());
    }
}

#[cfg(test)]
mod test_edge_signals {
    use ebur128::{EbuR128, Mode};

    use super::BlockMeter;
    use crate::golden::measure_signal;
    use crate::record::NEGATIVE_INFINITY_LOUDNESS;
    use crate::signals::{SignalKind, generate};

    /// Finds the zero-bounded sample extrema independently of meter blocks, ignoring NaNs and zero sign.
    fn reference_extrema(samples: &[f32]) -> (f32, f32) {
        let minimum = samples
            .iter()
            .copied()
            .filter(|sample| *sample < 0.0)
            .min_by(f32::total_cmp)
            .unwrap_or(0.0);
        let maximum = samples
            .iter()
            .copied()
            .filter(|sample| *sample > 0.0)
            .max_by(f32::total_cmp)
            .unwrap_or(0.0);
        (minimum, maximum)
    }

    #[test]
    fn empty_input_has_no_history_and_no_loudness() {
        let output = BlockMeter::new(2, 44_100, None).expect("meter").finish(0);
        assert_eq!(output.history, Vec::<f32>::new());
        assert_eq!(output.integrated_lufs.to_bits(), NEGATIVE_INFINITY_LOUDNESS.to_bits());
        assert_eq!(
            output.max_short_term_lufs.to_bits(),
            NEGATIVE_INFINITY_LOUDNESS.to_bits()
        );
        assert_eq!(output.true_peaks, vec![0.0, 0.0]);
    }

    #[test]
    fn a_partial_block_adds_one_reading() {
        for frames in [1, 4_409, 4_410, 4_411, 8_820] {
            let output = measure_signal(&generate(SignalKind::Sine, 44_100, 2, frames), 44_100, 512);
            assert_eq!(output.history.len(), frames.div_ceil(4_410), "{frames} frames");
        }
    }

    #[test]
    fn silence_reads_the_sentinel_everywhere() {
        let output = measure_signal(&generate(SignalKind::Silence, 48_000, 2, 96_000), 48_000, 1_000);
        assert_eq!(output.integrated_lufs.to_bits(), NEGATIVE_INFINITY_LOUDNESS.to_bits());
        assert!(
            output
                .history
                .iter()
                .all(|&value| f64::from(value) == NEGATIVE_INFINITY_LOUDNESS)
        );
        assert_eq!(output.true_peaks, vec![0.0, 0.0]);
    }

    #[test]
    fn dc_is_removed_by_the_k_weighting() {
        let output = measure_signal(&generate(SignalKind::Dc, 48_000, 2, 96_000), 48_000, 1_000);
        // An unfiltered 0.25 offset would read about -12 LUFS, only the transient of its onset remains.
        assert!(output.integrated_lufs < -30.0, "{}", output.integrated_lufs);
        // The step from the silence before the first sample rings in the interpolator, so the true peak overshoots.
        assert!(output.true_peaks[0] >= 0.25, "{}", output.true_peaks[0]);
    }

    #[test]
    fn non_finite_samples_do_not_panic() {
        let mut planes = generate(SignalKind::Sine, 44_100, 2, 44_100);
        planes[0][100] = f32::NAN;
        planes[1][200] = f32::NEG_INFINITY;
        let output = measure_signal(&planes, 44_100, 1_000);
        assert_eq!(output.history.len(), 10);
        assert_eq!(output.min_samples[1], f32::NEG_INFINITY);
    }

    #[test]
    fn sample_only_true_peaks_reuse_extremes_with_non_finite_samples() {
        let planes = vec![
            vec![f32::NAN, -0.0, f32::from_bits(1), -f32::from_bits(7)],
            vec![f32::INFINITY, f32::NEG_INFINITY, f32::NAN, 0.0],
            vec![f32::NAN; 4],
        ];
        let plane_refs: Vec<_> = planes.iter().map(Vec::as_slice).collect();
        for rate in [192_000, 384_000] {
            let output = measure_signal(&planes, rate, 1);
            let mut reference = EbuR128::new(3, rate, Mode::TRUE_PEAK).expect("reference meter");
            reference.add_frames_planar_f32(&plane_refs).expect("reference samples");
            for (channel, &peak) in output.true_peaks.iter().enumerate() {
                let expected = reference.true_peak(channel as u32).expect("reference peak");
                assert_eq!(peak.to_bits(), expected.to_bits(), "rate {rate}, channel {channel}");
            }
        }
    }

    #[test]
    fn block_extrema_preserve_scalar_bits_across_boundaries() {
        let samples = [
            f32::NAN,
            -0.0,
            0.0,
            -f32::from_bits(1),
            f32::from_bits(1),
            -1.0,
            1.0,
            f32::NEG_INFINITY,
            f32::INFINITY,
        ];
        for frames in [1, 7, 4_799, 4_800, 4_801, 9_603] {
            let plane: Vec<_> = (0..frames).map(|index| samples[index % samples.len()]).collect();
            let (minimum, maximum) = reference_extrema(&plane);
            let output = measure_signal(&[plane], 48_000, frames);
            assert_eq!(output.min_samples[0].to_bits(), minimum.to_bits());
            assert_eq!(output.max_samples[0].to_bits(), maximum.to_bits());
        }
        let mut negative_zero_then_nan = vec![-0.0; 4_800];
        negative_zero_then_nan.resize(9_603, f32::NAN);
        let mut alternating_zero_blocks = vec![-0.0; 4_800];
        alternating_zero_blocks.resize(9_600, 0.0);
        alternating_zero_blocks.resize(9_603, -0.0);
        for samples in [
            vec![-0.0; 9_603],
            vec![f32::NAN; 9_603],
            negative_zero_then_nan,
            alternating_zero_blocks,
        ] {
            let (minimum, maximum) = reference_extrema(&samples);
            for chunk_frames in [1_000, 4_800, 4_801, 9_603] {
                let output = measure_signal(std::slice::from_ref(&samples), 48_000, chunk_frames);
                assert_eq!(output.min_samples[0].to_bits(), minimum.to_bits());
                assert_eq!(output.max_samples[0].to_bits(), maximum.to_bits());
            }
        }
        for boundary in [4_799, 4_800, 4_801, 9_599, 9_600, 9_601] {
            let mut samples = vec![-0.0; 9_603];
            samples[boundary] = -0.75;
            samples[boundary + 1] = 0.5;
            let (minimum, maximum) = reference_extrema(&samples);
            assert_eq!(minimum.to_bits(), (-0.75_f32).to_bits());
            assert_eq!(maximum.to_bits(), 0.5_f32.to_bits());
            for chunk_frames in [1_000, 4_800, 4_801, 9_603] {
                let output = measure_signal(std::slice::from_ref(&samples), 48_000, chunk_frames);
                assert_eq!(output.min_samples[0].to_bits(), minimum.to_bits());
                assert_eq!(output.max_samples[0].to_bits(), maximum.to_bits());
            }
        }
    }

    #[test]
    fn a_full_scale_square_has_a_true_peak_above_its_sample_peak() {
        let output = measure_signal(&generate(SignalKind::Square, 44_100, 2, 44_100), 44_100, 1_000);
        assert_eq!(output.max_samples[0], 1.0);
        assert!(output.true_peaks[0] > 1.0, "{}", output.true_peaks[0]);
    }
}
