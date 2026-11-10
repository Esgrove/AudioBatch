//! Single-resolution stereo power analysis on a worker thread, independent of graphics and plugin APIs.

use std::sync::Arc;

use realfft::num_complex::Complex32;
use realfft::{RealFftPlanner, RealToComplex};
use thiserror::Error;

/// Window length for the intentionally single-tier spike analyzer.
const WINDOW_LENGTH: usize = 2_048;
/// Hop length measured in audio frames, independent of host block boundaries.
const HOP_LENGTH: usize = 512;

/// A validated spectral observation ready for worker-side GPU instance construction.
#[derive(Clone, Copy, Debug)]
pub struct SpectralPoint {
    frequency: f32,
    position: f32,
    power: f32,
}

/// Worker-side setup or transform errors, never formatted in an audio callback.
#[derive(Debug, Error)]
pub enum AnalysisError {
    /// An invalid rate must not enter logarithmic frequency projection.
    #[error("unsupported spatial analyzer sample rate")]
    InvalidRate,
    /// Transform sizes and scratch are fixed at construction, so this indicates an internal contract failure.
    #[error("spatial FFT failed: {0}")]
    Transform(#[from] realfft::FftError),
}

/// Retained worker-owned history and FFT scratch. Construction allocates, streaming reuses all storage.
pub struct SpatialAnalyzer {
    transform: Arc<dyn RealToComplex<f32>>,
    sample_rate: f32,
    history: Vec<[f32; 2]>,
    window: Vec<f32>,
    input: Vec<f32>,
    left: Vec<Complex32>,
    right: Vec<Complex32>,
    scratch: Vec<Complex32>,
    write_index: usize,
    frames: usize,
}

impl SpectralPoint {
    /// Returns frequency in Hz.
    #[must_use]
    pub const fn frequency(self) -> f32 {
        self.frequency
    }

    /// Returns top-to-bottom stereo position, zero for Left and one for Right.
    #[must_use]
    pub const fn position(self) -> f32 {
        self.position
    }

    /// Returns combined channel power normalized to an equal-channel full-scale interior sine.
    #[must_use]
    pub const fn power(self) -> f32 {
        self.power
    }
}

impl SpatialAnalyzer {
    /// Allocates one Hann-windowed FFT tier outside the audio callback.
    ///
    /// # Errors
    /// Rejects non-finite or unsupported rates.
    pub fn new(sample_rate: f32) -> Result<Self, AnalysisError> {
        if !sample_rate.is_finite() || !(8_000.0..=768_000.0).contains(&sample_rate) {
            return Err(AnalysisError::InvalidRate);
        }
        let transform = RealFftPlanner::new().plan_fft_forward(WINDOW_LENGTH);
        Ok(Self {
            sample_rate,
            history: vec![[0.0; 2]; WINDOW_LENGTH],
            window: (0..WINDOW_LENGTH)
                .map(|index| {
                    (-0.5_f32).mul_add((std::f32::consts::TAU * index as f32 / WINDOW_LENGTH as f32).cos(), 0.5)
                })
                .collect(),
            input: transform.make_input_vec(),
            left: transform.make_output_vec(),
            right: transform.make_output_vec(),
            scratch: transform.make_scratch_vec(),
            transform,
            write_index: 0,
            frames: 0,
        })
    }

    /// Clears worker history at a stream discontinuity without reallocating.
    pub fn reset(&mut self) {
        self.history.fill([0.0; 2]);
        self.write_index = 0;
        self.frames = 0;
    }

    /// Appends observations at exact hop endpoints to the caller's reusable worker storage.
    /// Non-finite input clears history rather than poisoning subsequent frames.
    ///
    /// # Errors
    /// Returns a transform error if its fixed buffer contract is violated.
    pub fn push(&mut self, frames: &[[f32; 2]], points: &mut Vec<SpectralPoint>) -> Result<(), AnalysisError> {
        for &frame in frames {
            if !frame.iter().all(|sample| sample.is_finite()) {
                self.reset();
                continue;
            }
            if let Some(slot) = self.history.get_mut(self.write_index) {
                *slot = frame;
            }
            self.write_index = (self.write_index + 1) % WINDOW_LENGTH;
            self.frames += 1;
            if self.frames >= WINDOW_LENGTH && self.frames.is_multiple_of(HOP_LENGTH) {
                self.analyze(points)?;
            }
        }
        Ok(())
    }

    /// Computes both channel spectra with preallocated scratch on the worker thread.
    fn analyze(&mut self, points: &mut Vec<SpectralPoint>) -> Result<(), AnalysisError> {
        for (channel, spectrum) in [(0, &mut self.left), (1, &mut self.right)] {
            for (index, (destination, &weight)) in self.input.iter_mut().zip(&self.window).enumerate() {
                let frame = self
                    .history
                    .get((self.write_index + index) % WINDOW_LENGTH)
                    .copied()
                    .unwrap_or_default();
                *destination = frame.get(channel).copied().unwrap_or_default() * weight;
            }
            self.transform
                .process_with_scratch(&mut self.input, spectrum, &mut self.scratch)?;
        }
        let normalization = 8.0 / (WINDOW_LENGTH * WINDOW_LENGTH) as f32;
        for (bin, (left, right)) in self.left.iter().zip(&self.right).enumerate().skip(1) {
            let frequency = bin as f32 * self.sample_rate / WINDOW_LENGTH as f32;
            if !(20.0..=20_000.0).contains(&frequency) {
                continue;
            }
            let left_power = left.norm_sqr();
            let right_power = right.norm_sqr();
            let power = (left_power + right_power) * normalization;
            if !power.is_finite() || power < 1.0e-12 {
                continue;
            }
            let difference_db = 10.0 * ((right_power + 1.0e-20) / (left_power + 1.0e-20)).log10();
            let position = 0.5_f32.mul_add((difference_db / 20.0).tanh(), 0.5);
            points.push(SpectralPoint {
                frequency,
                position,
                power,
            });
        }
        Ok(())
    }
}

#[cfg(test)]
mod test_analysis {
    use super::SpatialAnalyzer;

    /// Builds a bin-centered stereo sine, avoiding ambiguous frequency expectations.
    fn signal(left: f32, right: f32) -> Vec<[f32; 2]> {
        (0..4_096)
            .map(|index| {
                let sine = (std::f32::consts::TAU * 32.0 * index as f32 / 2_048.0).sin();
                [sine * left, sine * right]
            })
            .collect()
    }

    #[test]
    fn mono_and_antiphase_remain_centered() {
        for right in [1.0, -1.0] {
            let mut analyzer = SpatialAnalyzer::new(48_000.0).expect("rate");
            let mut points = Vec::new();
            analyzer.push(&signal(1.0, right), &mut points).expect("FFT");
            let peak = points
                .iter()
                .max_by(|a, b| a.power().total_cmp(&b.power()))
                .expect("peak");
            assert!((peak.frequency() - 750.0).abs() < 0.001);
            assert!((peak.position() - 0.5).abs() < 0.001);
            assert!((peak.power() - 1.0).abs() < 0.001);
        }
    }

    #[test]
    fn left_only_maps_to_top() {
        let mut analyzer = SpatialAnalyzer::new(48_000.0).expect("rate");
        let mut points = Vec::new();
        analyzer.push(&signal(1.0, 0.0), &mut points).expect("FFT");
        let peak = points
            .iter()
            .max_by(|a, b| a.power().total_cmp(&b.power()))
            .expect("peak");
        assert!(peak.position() < 0.001);
    }

    #[test]
    fn hop_endpoints_do_not_depend_on_chunk_sizes() {
        let frames = signal(0.25, 0.5);
        let mut whole = SpatialAnalyzer::new(48_000.0).expect("rate");
        let mut split = SpatialAnalyzer::new(48_000.0).expect("rate");
        let mut expected = Vec::new();
        let mut actual = Vec::new();
        whole.push(&frames, &mut expected).expect("FFT");
        for chunk in frames.chunks(37) {
            split.push(chunk, &mut actual).expect("FFT");
        }
        assert_eq!(expected.len(), actual.len());
        for (a, b) in expected.iter().zip(actual) {
            assert_eq!(a.frequency().to_bits(), b.frequency().to_bits());
            assert_eq!(a.power().to_bits(), b.power().to_bits());
            assert_eq!(a.position().to_bits(), b.position().to_bits());
        }
    }

    #[test]
    fn silence_and_nonfinite_samples_produce_no_invalid_points() {
        let mut analyzer = SpatialAnalyzer::new(48_000.0).expect("rate");
        let mut points = Vec::new();
        analyzer.push(&vec![[0.0, 0.0]; 4_096], &mut points).expect("FFT");
        analyzer.push(&[[f32::NAN, 0.0]], &mut points).expect("FFT");
        assert!(points.is_empty());
        assert!(SpatialAnalyzer::new(f32::INFINITY).is_err());
    }
}
