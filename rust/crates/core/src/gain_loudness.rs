//! Isolated gain and hard-clip loudness predictions, without source records, caches, or plugin processing.

use std::path::Path;

use crate::decode::{AudioFileDecoder, DecodeError};
use crate::loudness::gating::LoudnessMeter;
use crate::meter::MeterError;
use crate::output_file::SourceIdentity;
use crate::record::{NEGATIVE_INFINITY_LOUDNESS, ShortTermLoudnessHistory, decibels_to_gain, normalize_loudness};

/// Empirical extra attenuation scale fitted to the saved L2 preset's 24 music measurements.
const LIMITER_LOSS_SCALE: f64 = 1.348_139_672_181_545_7;
/// Empirical exponent of hard-clipping loudness loss, in LU.
const LIMITER_LOSS_EXPONENT: f64 = 0.703_590_411_233_577_4;
/// Unity-gain loudness reduction of the calibration preset with its -0.3 dB output ceiling.
const LIMITER_CEILING_OFFSET: f64 = -0.3;
/// Largest input gain in the calibration sweep, above which hard clipping remains the fallback.
const LIMITER_MAXIMUM_GAIN_DB: f32 = 6.0;

/// A failed prediction never establishes source measurement or cache authority.
#[derive(Debug, thiserror::Error)]
pub enum GainLoudnessError {
    /// The input could not be decoded.
    #[error(transparent)]
    Decode(#[from] DecodeError),
    /// The stream is outside the measurement contract.
    #[error(transparent)]
    Meter(#[from] MeterError),
    /// The source identity could not be captured.
    #[error("Could not identify the prediction source")]
    Identity(#[from] std::io::Error),
    /// Gain must be finite and representable as a linear multiplier.
    #[error("Invalid prediction gain")]
    InvalidGain,
    /// Non-finite decoded PCM cannot establish a meaningful loudness prediction.
    #[error("Non-finite decoded sample")]
    InvalidPcm,
    /// The caller no longer needs the result.
    #[error("Loudness prediction was cancelled")]
    Cancelled,
    /// Source bytes changed during the read.
    #[error("Loudness prediction source changed")]
    SourceChanged,
}

/// Loudness of decoded samples multiplied by custom gain and hard clipped at ±1.
/// This is not a prediction of any downstream limiter or plugin chain.
#[derive(Debug, Clone, Copy)]
pub struct GainLoudness {
    integrated: f64,
    max_short_term: f64,
}

/// Buffers only one aligned 100 ms block, keeping the normal loudness window and gate semantics.
struct GainedMeter {
    meter: LoudnessMeter,
    block: Vec<Vec<f32>>,
    block_size: usize,
    gain: f64,
    max_short_term: f64,
}

impl GainLoudness {
    /// Returns gated integrated loudness, including the usual silence sentinel.
    #[must_use]
    pub const fn integrated(self) -> f64 {
        self.integrated
    }

    /// Returns the maximum zero-padded three-second loudness.
    #[must_use]
    pub const fn max_short_term(self) -> f64 {
        self.max_short_term
    }
}

impl GainedMeter {
    /// Creates a loudness-only meter, without true peak, waveform, spectrum, or history allocations.
    fn new(channels: u32, sample_rate: u32, gain_db: f32) -> Result<Self, GainLoudnessError> {
        let gain = decibels_to_gain(f64::from(gain_db));
        if !gain_db.is_finite() || !gain.is_finite() {
            return Err(GainLoudnessError::InvalidGain);
        }
        if !(1..=64).contains(&channels) || !(16..=2_822_400).contains(&sample_rate) {
            return Err(MeterError::UnsupportedStream.into());
        }
        let block_size = ShortTermLoudnessHistory::samples_per_point_for_rate(sample_rate) as usize;
        Ok(Self {
            meter: LoudnessMeter::new(channels as usize, sample_rate, None),
            block: vec![Vec::with_capacity(block_size); channels as usize],
            block_size,
            gain,
            max_short_term: NEGATIVE_INFINITY_LOUDNESS,
        })
    }

    /// Applies gain before the nonlinearity and K weighting, preserving channel weights and gate recalculation.
    fn push(&mut self, chunk: &[Vec<f32>]) -> Result<(), GainLoudnessError> {
        let frames = chunk.first().map_or(0, Vec::len);
        let mut offset = 0;
        while offset < frames {
            let filled = self.block.first().expect("validated channels").len();
            let take = (self.block_size - filled).min(frames - offset);
            for (output, input) in self.block.iter_mut().zip(chunk) {
                for &sample in input
                    .get(offset..offset + take)
                    .expect("decoder planes have equal lengths")
                {
                    if !sample.is_finite() {
                        return Err(GainLoudnessError::InvalidPcm);
                    }
                    // Shared with the C++ preview model, multiply and clamp in double precision,
                    // then round once to the float PCM consumed by the loudness meter.
                    output.push((f64::from(sample) * self.gain).clamp(-1.0, 1.0) as f32);
                }
            }
            offset += take;
            if filled + take == self.block_size {
                self.measure_block();
            }
        }
        Ok(())
    }

    /// Measures a full block or the final partial block without creating an incomplete integrated gate.
    fn measure_block(&mut self) {
        let mut planes = [&[][..]; 64];
        for (plane, samples) in planes.iter_mut().zip(&self.block) {
            *plane = samples;
        }
        self.max_short_term = self.max_short_term.max(normalize_loudness(
            self.meter
                .push(planes.get(..self.block.len()).expect("validated channels")),
        ));
        for plane in &mut self.block {
            plane.clear();
        }
    }

    /// Finalizes only the two requested loudness scalars.
    fn finish(mut self) -> GainLoudness {
        if self.block.first().is_some_and(|plane| !plane.is_empty()) {
            self.measure_block();
        }
        GainLoudness {
            integrated: normalize_loudness(self.meter.integrated()),
            max_short_term: self.max_short_term,
        }
    }
}

/// Estimates integrated limiter output from source loudness and a measured gain-and-clip result.
///
/// The empirical L2 fit is reproducible with `fixtures/limiter_model.py` and assumes a -0.3 dB output ceiling.
/// Calibration covers four tracks at -13.33 through -8.68 LUFS-I, with 0 through +6 dB gain.
/// Other source loudness extrapolates the fit, so this remains an estimate rather than a plugin measurement.
/// Negative gains, gains above the sweep, invalid inputs, and silence retain the hard-clipping result.
#[must_use]
pub fn estimate_limiter_integrated(source_lufs: f64, gain_db: f32, clipped_lufs: f64) -> f64 {
    if !source_lufs.is_finite()
        || !clipped_lufs.is_finite()
        || source_lufs <= NEGATIVE_INFINITY_LOUDNESS
        || clipped_lufs <= NEGATIVE_INFINITY_LOUDNESS
        || !(0.0..=LIMITER_MAXIMUM_GAIN_DB).contains(&gain_db)
    {
        return clipped_lufs;
    }
    let loss = (source_lufs + f64::from(gain_db) - clipped_lufs).max(0.0);
    LIMITER_LOSS_SCALE.mul_add(-loss.powf(LIMITER_LOSS_EXPONENT), clipped_lufs + LIMITER_CEILING_OFFSET)
}

/// Remeasures decoded samples after gain and hard clipping at 0 dBFS.
///
/// Runs on a caller-owned worker and checks cancellation at each decoder packet.
/// No source record or cache is read or written, and no output audio is produced.
///
/// # Errors
///
/// Returns decoding, stream, invalid-gain, non-finite PCM, cancellation, or changed-source errors.
pub fn measure_gained_loudness(
    path: &Path,
    gain_db: f32,
    is_cancelled: &dyn Fn() -> bool,
) -> Result<GainLoudness, GainLoudnessError> {
    if is_cancelled() {
        return Err(GainLoudnessError::Cancelled);
    }
    let identity = SourceIdentity::capture(path)?;
    let mut decoder = AudioFileDecoder::open(path)?;
    let mut meter = GainedMeter::new(decoder.info().channels, decoder.info().sample_rate, gain_db)?;
    loop {
        if is_cancelled() {
            return Err(GainLoudnessError::Cancelled);
        }
        let Some(chunk) = decoder.next_chunk()? else {
            break;
        };
        meter.push(chunk)?;
    }
    if is_cancelled() {
        return Err(GainLoudnessError::Cancelled);
    }
    if SourceIdentity::capture(path)? != identity {
        return Err(GainLoudnessError::SourceChanged);
    }
    Ok(meter.finish())
}

#[cfg(test)]
mod test_limiter_fit {
    use super::*;

    #[test]
    fn fitted_values_match_the_independent_four_track_report() {
        for (source, clipped, expected) in [
            (-10.8243, -8.9767, -9.635_531_818_443_239),
            (-11.1205, -9.1929, -9.705_448_418_509_31),
            (-8.6760, -6.7819, -7.359_654_457_001_012),
            (-13.3286, -11.3533, -11.753_035_984_135_359),
        ] {
            assert!((estimate_limiter_integrated(source, 2.0, clipped) - expected).abs() < 1e-10);
        }
    }

    #[test]
    fn unsupported_gains_invalid_sources_and_silence_keep_the_clipping_result() {
        for gain in [-1.0, 6.01, f32::NAN, f32::INFINITY] {
            assert!((estimate_limiter_integrated(-12.0, gain, -10.1) + 10.1).abs() < f64::EPSILON);
        }
        assert!((estimate_limiter_integrated(f64::NAN, 2.0, -10.1) + 10.1).abs() < f64::EPSILON);
        assert!((estimate_limiter_integrated(NEGATIVE_INFINITY_LOUDNESS, 2.0, -10.1) + 10.1).abs() < f64::EPSILON);
        assert!(
            (estimate_limiter_integrated(-12.0, 2.0, NEGATIVE_INFINITY_LOUDNESS) - NEGATIVE_INFINITY_LOUDNESS).abs()
                < f64::EPSILON
        );
    }

    #[test]
    fn no_clipping_keeps_the_preset_ceiling_and_negative_losses_are_bounded() {
        assert!((estimate_limiter_integrated(-12.0, 2.0, -10.0) + 10.3).abs() < 1e-12);
        assert!((estimate_limiter_integrated(-12.0, 0.0, -12.0) + 12.3).abs() < 1e-12);
        assert!((estimate_limiter_integrated(-12.0, 2.0, -9.99) + 10.29).abs() < 1e-12);
        assert!(estimate_limiter_integrated(-12.0, 6.0, -6.5) < -6.8);
    }
}

#[cfg(test)]
mod test_gained_loudness {
    use std::f32::consts::TAU;

    use crate::meter::BlockMeter;
    use crate::record::is_silent_loudness;

    use super::*;

    /// Generates independent sine input lasting beyond the full short-term window.
    fn signal(amplitude: f32) -> Vec<Vec<f32>> {
        vec![
            (0..64_123)
                .map(|frame| amplitude * (TAU * 1000.0 * frame as f32 / 16_000.0).sin())
                .collect(),
        ]
    }

    /// Feeds deliberately unaligned chunks through the production gain path.
    fn gained(signal: &[Vec<f32>], gain_db: f32) -> GainLoudness {
        let mut meter = GainedMeter::new(1, 16_000, gain_db).expect("meter");
        for chunk in signal[0].chunks(997) {
            meter.push(&[chunk.to_vec()]).expect("finite signal");
        }
        meter.finish()
    }

    #[test]
    fn clipping_matches_independently_transformed_samples_and_not_naive_gain() {
        let input = signal(0.8);
        let gain_db = 12.0;
        let actual = gained(&input, gain_db);
        let multiplier = decibels_to_gain(f64::from(gain_db));
        let clipped: Vec<_> = input[0]
            .iter()
            .map(|&sample| (f64::from(sample) * multiplier).clamp(-1.0, 1.0) as f32)
            .collect();
        let mut reference = BlockMeter::new(1, 16_000, None).expect("reference meter");
        reference.push(&[clipped]);
        let expected = reference.finish(input[0].len() as u64);
        assert!((actual.integrated() - expected.integrated_lufs).abs() < 1e-10);
        assert!((actual.max_short_term() - expected.max_short_term_lufs).abs() < 1e-10);
        let source = gained(&input, 0.0);
        assert!(source.integrated() + f64::from(gain_db) - actual.integrated() > 4.0);
        assert!(source.max_short_term() + f64::from(gain_db) - actual.max_short_term() > 4.0);
    }

    #[test]
    fn unclipped_gain_and_silence_keep_normal_loudness_semantics() {
        let input = signal(0.1);
        let source = gained(&input, 0.0);
        let actual = gained(&input, 6.0);
        assert!((actual.integrated() - source.integrated() - 6.0).abs() < 1e-6);
        assert!((actual.max_short_term() - source.max_short_term() - 6.0).abs() < 1e-6);
        let silent = gained(&[vec![0.0; 64_000]], 12.0);
        assert!(is_silent_loudness(silent.integrated()));
        assert!(is_silent_loudness(silent.max_short_term()));
    }

    #[test]
    fn absolute_gate_is_recomputed_after_gain() {
        let input = signal(0.000_1);
        let source = gained(&input, 0.0);
        let actual = gained(&input, 24.0);
        assert!(is_silent_loudness(source.integrated()));
        assert!(!is_silent_loudness(actual.integrated()));
    }

    #[test]
    fn shared_double_precision_transform_has_independently_computed_pcm_bits() {
        // Binary32 samples promoted to binary64, multiplied by 10^(12/20), clamped, then rounded once.
        // These bounded known answers distinguish the shared C++ operation from a binary32 multiplier.
        let inputs = [
            0x3bc7_e282,
            0x3c07_fcb9,
            0x3c47_e282,
            0x3c87_fcb9,
            0xbbc7_e282,
            0x3f00_0000,
            0xbf00_0000,
        ];
        let expected = [
            0x3cc6_f05d,
            0x3d07_57fc,
            0x3d46_f05d,
            0x3d87_57fc,
            0xbcc6_f05d,
            0x3f80_0000,
            0xbf80_0000,
        ];
        let mut meter = GainedMeter::new(1, 16_000, 12.0).expect("meter");
        meter
            .push(&[inputs.into_iter().map(f32::from_bits).collect()])
            .expect("finite PCM");
        let actual: Vec<_> = meter
            .block
            .first()
            .expect("channel")
            .iter()
            .map(|sample| sample.to_bits())
            .collect();
        assert_eq!(actual, expected);
    }

    #[test]
    fn nonfinite_pcm_is_rejected_before_clipping_can_hide_it() {
        for invalid in [f32::NAN, f32::INFINITY, f32::NEG_INFINITY] {
            for gain in [-12.0, 0.0, 12.0] {
                let mut meter = GainedMeter::new(2, 16_000, gain).expect("meter");
                assert!(matches!(
                    meter.push(&[vec![0.1, 0.2], vec![0.0, invalid]]),
                    Err(GainLoudnessError::InvalidPcm)
                ));
            }
        }
    }

    #[test]
    fn cancellation_and_bad_gain_cannot_return_a_prediction() {
        let path = test_fixtures::fixture("sine_stereo_44100_16.wav");
        assert!(matches!(
            measure_gained_loudness(&path, 6.0, &|| true),
            Err(GainLoudnessError::Cancelled)
        ));
        assert!(matches!(
            measure_gained_loudness(&path, f32::NAN, &|| false),
            Err(GainLoudnessError::InvalidGain)
        ));
        let checks = std::cell::Cell::new(0);
        assert!(matches!(
            measure_gained_loudness(&path, 12.0, &|| {
                checks.set(checks.get() + 1);
                checks.get() > 3
            }),
            Err(GainLoudnessError::Cancelled)
        ));
        assert!(measure_gained_loudness(Path::new("/missing/prediction.wav"), 6.0, &|| false).is_err());
    }
}
