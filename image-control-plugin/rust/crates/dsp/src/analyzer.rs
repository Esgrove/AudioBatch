//! Six-tier, packed-stereo spectral analysis with audio-time deadlines and reusable worker-owned storage.

use std::sync::Arc;

use realfft::num_complex::Complex32;
use rustfft::{Fft, FftPlanner};
use thiserror::Error;

/// Number of independent frequency tiers, ordered from bass to treble.
pub const ANALYSIS_TIER_COUNT: usize = 6;
const MIN_FREQUENCY: f64 = 20.0;
const MAX_FREQUENCY: f64 = 20_000.0;
const SILENCE_POWER: f32 = 1.0e-12;
const REFERENCE_RATE: f32 = 48_000.0;
const POSITION_SMOOTHING_OCTAVES: f64 = 1.0 / 32.0;
const TIER_LAYOUTS: [TierLayout; ANALYSIS_TIER_COUNT] = [
    TierLayout {
        window_length: 32_768,
        hop: 2048,
        min_frequency: 0.0,
        max_frequency: 80.0,
    },
    TierLayout {
        window_length: 16_384,
        hop: 1024,
        min_frequency: 80.0,
        max_frequency: 160.0,
    },
    TierLayout {
        window_length: 8192,
        hop: 512,
        min_frequency: 160.0,
        max_frequency: 320.0,
    },
    TierLayout {
        window_length: 4096,
        hop: 512,
        min_frequency: 320.0,
        max_frequency: 640.0,
    },
    TierLayout {
        window_length: 2048,
        hop: 512,
        min_frequency: 640.0,
        max_frequency: 4000.0,
    },
    TierLayout {
        window_length: 1024,
        hop: 512,
        min_frequency: 4000.0,
        max_frequency: f64::INFINITY,
    },
];

/// One validated spectral observation. Position is normalized from full left (0) to full right (1).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SpectralPoint {
    frequency: f32,
    reassigned_frequency: f32,
    spacing: f32,
    position: f32,
    power: f32,
    coherence: f32,
}

/// Audio-time identity of one tier frame, independent of caller block boundaries.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SpectralFrameInfo {
    /// Frequency tier index, from zero to five.
    pub tier_index: usize,
    /// Duration of this tier's hop in seconds.
    pub frame_seconds: f64,
    /// Exclusive audio endpoint since reset, in seconds.
    pub end_seconds: f64,
    /// Whether this is the last tier due at the shared audio endpoint.
    pub last_at_time: bool,
}

/// Worker-side setup errors, never formatted in an audio callback.
#[derive(Debug, Error)]
pub enum AnalysisError {
    /// Rates must be finite and lie between 8 kHz and 768 kHz inclusive.
    #[error("unsupported spatial analyzer sample rate")]
    InvalidRate,
}

/// Validated level-difference curve, matching the reference midpoint and edge controls.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct StereoPositionCurve {
    exponent: f32,
    scale_decibels: f32,
}

/// Unscaled window geometry and reported frequency range for one analysis tier.
#[derive(Clone, Copy)]
struct TierLayout {
    /// Number of history samples in the base-rate analysis window.
    window_length: usize,
    /// Number of base-rate samples between observation deadlines.
    hop: usize,
    /// Inclusive lower bound of the tier's reported frequency range in Hz.
    min_frequency: f64,
    /// Upper bound of the reported range in Hz, further limited by Nyquist and the display range.
    max_frequency: f64,
}

/// One transform and its fixed spectral-neighborhood work storage.
struct AnalysisTier {
    transform: Arc<dyn Fft<f32>>,
    spectrum: Vec<Complex32>,
    scratch: Vec<Complex32>,
    bins: Vec<AnalyzedBin>,
    window_length: usize,
    hop: usize,
    since_frame: usize,
    first_bin: usize,
    bin_width: f64,
    level_scale: f32,
}

/// Static bin geometry and dynamic per-frame powers share a preallocated tier-local buffer.
struct AnalyzedBin {
    frequency: f32,
    first_neighbor: usize,
    last_neighbor: usize,
    left_power: f32,
    right_power: f32,
    cross_power: f32,
    reassigned_frequency: f32,
}

/// Worker-owned analyzer. Construction allocates. Streaming and reset reuse all internal storage.
pub struct SpatialAnalyzer {
    tiers: Vec<AnalysisTier>,
    history: Vec<[f32; 2]>,
    points: Vec<SpectralPoint>,
    curve: StereoPositionCurve,
    sample_rate: f64,
    write_index: usize,
    processed_samples: u64,
}

impl SpectralPoint {
    /// Validates an independently supplied observation before projection or logarithmic level calculations.
    #[must_use]
    pub fn new(
        frequency: f32,
        reassigned_frequency: f32,
        spacing: f32,
        position: f32,
        power: f32,
        coherence: f32,
    ) -> Option<Self> {
        ([frequency, reassigned_frequency, spacing, position, power, coherence]
            .into_iter()
            .all(f32::is_finite)
            && (20.0..=20_000.0).contains(&frequency)
            && (20.0..=20_000.0).contains(&reassigned_frequency)
            && spacing >= 0.0
            && (0.0..=1.0).contains(&position)
            && power > 0.0
            && (-1.0..=1.0).contains(&coherence))
        .then_some(Self {
            frequency,
            reassigned_frequency,
            spacing,
            position,
            power,
            coherence,
        })
    }

    /// Returns the original FFT bin frequency in Hz, used for level tilt and palette lookup.
    #[must_use]
    pub const fn frequency(self) -> f32 {
        self.frequency
    }

    /// Returns the bounded instantaneous frequency in Hz, used only for horizontal placement.
    #[must_use]
    pub const fn reassigned_frequency(self) -> f32 {
        self.reassigned_frequency
    }

    /// Returns the zero-padded transform bin spacing in Hz.
    #[must_use]
    pub const fn spacing(self) -> f32 {
        self.spacing
    }

    /// Returns top-to-bottom stereo position, zero for Left and one for Right.
    #[must_use]
    pub const fn position(self) -> f32 {
        self.position
    }

    /// Returns combined channel power, calibrated so a full-scale interior sine in one channel reads one.
    #[must_use]
    pub const fn power(self) -> f32 {
        self.power
    }

    /// Returns neighborhood phase coherence from minus one (opposite polarity) to plus one (in phase).
    #[must_use]
    pub const fn coherence(self) -> f32 {
        self.coherence
    }
}

impl Default for StereoPositionCurve {
    /// Uses the reference 12 dB midpoint and 32 dB edge settings.
    fn default() -> Self {
        Self::from_valid_settings(12.0, 32.0)
    }
}

impl StereoPositionCurve {
    /// Validates finite reference setting ranges, including the minimum 3 dB midpoint-to-edge gap.
    #[must_use]
    pub fn new(midpoint_decibels: f64, edge_decibels: f64) -> Option<Self> {
        (midpoint_decibels.is_finite()
            && edge_decibels.is_finite()
            && (3.0..=24.0).contains(&midpoint_decibels)
            && (12.0..=60.0).contains(&edge_decibels)
            && edge_decibels >= midpoint_decibels + 3.0)
            .then(|| Self::from_valid_settings(midpoint_decibels, edge_decibels))
    }

    /// Derives the curve once when settings change, never in repeated bin processing.
    fn from_valid_settings(midpoint: f64, edge: f64) -> Self {
        let argument = 0.5_f64.atanh();
        let exponent = (0.99_f64.atanh() / argument).log(edge / midpoint);
        Self {
            exponent: exponent as f32,
            scale_decibels: (midpoint / argument.powf(1.0 / exponent)) as f32,
        }
    }

    /// Converts neighborhood powers to normalized position, retaining exact one-channel edge behavior.
    fn position(self, left_power: f32, right_power: f32) -> f32 {
        if right_power <= 0.0 {
            return if left_power > 0.0 { 0.0 } else { 0.5 };
        }
        if left_power <= 0.0 {
            return 1.0;
        }
        let difference = 20.0 * (left_power.sqrt() / right_power.sqrt()).log10();
        let share = (difference.abs() / self.scale_decibels).powf(self.exponent).tanh();
        (1.0 - share.copysign(difference)) * 0.5
    }
}

impl SpatialAnalyzer {
    /// Allocates six packed transforms and their reusable scratch outside the audio callback.
    ///
    /// # Errors
    /// Rejects non-finite rates and rates outside 8 kHz through 768 kHz.
    pub fn new(sample_rate: f32) -> Result<Self, AnalysisError> {
        if !sample_rate.is_finite() || !(8000.0..=768_000.0).contains(&sample_rate) {
            return Err(AnalysisError::InvalidRate);
        }
        let mut scale = 1;
        for _ in 0..4 {
            if sample_rate <= REFERENCE_RATE * scale as f32 {
                break;
            }
            scale *= 2;
        }
        let mut planner = FftPlanner::new();
        let tiers: Vec<_> = TIER_LAYOUTS
            .into_iter()
            .map(|layout| AnalysisTier::new(layout, scale, f64::from(sample_rate), &mut planner))
            .collect();
        let longest = 32_768 * scale;
        let most_points = tiers.iter().map(|tier| tier.bins.len()).max().unwrap_or_default();
        Ok(Self {
            tiers,
            history: vec![[0.0; 2]; longest],
            points: Vec::with_capacity(most_points),
            curve: StereoPositionCurve::default(),
            sample_rate: f64::from(sample_rate),
            write_index: 0,
            processed_samples: 0,
        })
    }

    /// Clears history and restarts the audio clock without reallocating. First frames arrive at the first hop.
    pub fn reset(&mut self) {
        self.history.fill([0.0; 2]);
        self.points.clear();
        for tier in &mut self.tiers {
            tier.since_frame = 0;
        }
        self.write_index = 0;
        self.processed_samples = 0;
    }

    /// Updates validated position coefficients without rebuilding transforms or clearing spectral history.
    pub const fn set_position_curve(&mut self, curve: StereoPositionCurve) {
        self.curve = curve;
    }

    /// Appends all observations for compatibility. Streaming clients should use [`Self::push_frames`].
    ///
    /// # Errors
    /// Retains the analyzer's fallible streaming interface for callers sharing setup-error handling.
    pub fn push(&mut self, frames: &[[f32; 2]], points: &mut Vec<SpectralPoint>) -> Result<(), AnalysisError> {
        self.push_frames(frames, |frame_points, _| points.extend_from_slice(frame_points))
    }

    /// Delivers reused point slices at exact tier endpoints, including silent frames and zero-history startup.
    /// Simultaneous frames arrive low-to-high. The slice is borrowed only for the callback duration.
    /// Non-finite input samples become silence in display history without altering the audio timeline.
    ///
    /// # Errors
    /// Retains a fallible interface for worker recovery without formatting errors in audio processing.
    pub fn push_frames(
        &mut self,
        frames: &[[f32; 2]],
        mut callback: impl FnMut(&[SpectralPoint], SpectralFrameInfo),
    ) -> Result<(), AnalysisError> {
        let mut remaining = frames;
        let mask = self.history.len() - 1;
        while !remaining.is_empty() {
            let count = self
                .tiers
                .iter()
                .map(|tier| tier.hop - tier.since_frame)
                .min()
                .unwrap_or(remaining.len())
                .min(remaining.len());
            let (current, rest) = remaining.split_at(count);
            for &frame in current {
                if let Some(slot) = self.history.get_mut(self.write_index) {
                    *slot = frame.map(|sample| if sample.is_finite() { sample } else { 0.0 });
                }
                self.write_index = (self.write_index + 1) & mask;
            }
            self.processed_samples += count as u64;
            remaining = rest;
            let mut last_due = None;
            for (index, tier) in self.tiers.iter_mut().enumerate() {
                tier.since_frame += count;
                if tier.since_frame == tier.hop {
                    last_due = Some(index);
                }
            }
            for (index, tier) in self.tiers.iter_mut().enumerate() {
                if tier.since_frame != tier.hop {
                    continue;
                }
                tier.since_frame = 0;
                tier.analyze(&self.history, self.write_index, self.curve, &mut self.points);
                callback(
                    &self.points,
                    SpectralFrameInfo {
                        tier_index: index,
                        frame_seconds: tier.hop as f64 / self.sample_rate,
                        end_seconds: self.processed_samples as f64 / self.sample_rate,
                        last_at_time: Some(index) == last_due,
                    },
                );
            }
        }
        Ok(())
    }
}

impl AnalysisTier {
    /// Plans one zero-padded packed transform and precomputes all reported bin geometry.
    fn new(layout: TierLayout, scale: usize, sample_rate: f64, planner: &mut FftPlanner<f32>) -> Self {
        let window_length = layout.window_length * scale;
        let fft_size = window_length * 2;
        let transform = planner.plan_fft_forward(fft_size);
        let bin_width = sample_rate / fft_size as f64;
        let first_bin = ((layout.min_frequency.max(MIN_FREQUENCY) / bin_width).ceil() as usize).max(1);
        let last_bin = ((layout.max_frequency.min(MAX_FREQUENCY) / bin_width).floor() as usize).min(window_length - 1);
        let bin_count = (last_bin + 1).saturating_sub(first_bin);
        let smoothing_ratio = (POSITION_SMOOTHING_OCTAVES * 0.5).exp2() - 1.0;
        let bins = (0..bin_count)
            .map(|index| {
                let frequency = (first_bin + index) as f64 * bin_width;
                let reach = (frequency * smoothing_ratio / bin_width).floor() as usize;
                AnalyzedBin {
                    frequency: frequency as f32,
                    first_neighbor: index.saturating_sub(reach),
                    last_neighbor: (index + reach).min(bin_count - 1),
                    left_power: 0.0,
                    right_power: 0.0,
                    cross_power: 0.0,
                    reassigned_frequency: frequency as f32,
                }
            })
            .collect();
        Self {
            scratch: vec![Complex32::default(); transform.get_inplace_scratch_len()],
            spectrum: vec![Complex32::default(); fft_size],
            transform,
            bins,
            window_length,
            hop: layout.hop * scale,
            since_frame: 0,
            first_bin,
            bin_width,
            level_scale: (16.0 / (window_length as f64).powi(2)) as f32,
        }
    }

    /// Applies Hann and derivative three-tap spectral filters after a single packed stereo FFT.
    fn analyze(
        &mut self,
        history: &[[f32; 2]],
        write_index: usize,
        curve: StereoPositionCurve,
        points: &mut Vec<SpectralPoint>,
    ) {
        let mask = history.len() - 1;
        let start = (write_index + history.len() - self.window_length) & mask;
        for (index, destination) in self.spectrum.iter_mut().take(self.window_length).enumerate() {
            let [left, right] = history.get((start + index) & mask).copied().unwrap_or_default();
            *destination = Complex32::new(left, right);
        }
        for destination in self.spectrum.iter_mut().skip(self.window_length) {
            *destination = Complex32::default();
        }
        self.transform
            .process_with_scratch(&mut self.spectrum, &mut self.scratch);
        let derivative_scale = (std::f64::consts::PI / self.window_length as f64) as f32;
        let radians_to_hertz = self.bin_width * self.spectrum.len() as f64 / std::f64::consts::TAU;
        let lobe_half_width = 4.0 * self.bin_width;
        let spectrum_mask = self.spectrum.len() - 1;
        for (index, bin) in self.bins.iter_mut().enumerate() {
            let center = self.first_bin + index;
            let (left, right) = separate_channels(&self.spectrum, center);
            let (low_left, low_right) =
                separate_channels(&self.spectrum, (center + self.spectrum.len() - 2) & spectrum_mask);
            let (high_left, high_right) = separate_channels(&self.spectrum, (center + 2) & spectrum_mask);
            let hann_left = left * 0.5 - (low_left + high_left) * 0.25;
            let hann_right = right * 0.5 - (low_right + high_right) * 0.25;
            let difference_left = low_left - high_left;
            let difference_right = low_right - high_right;
            let derivative_left =
                Complex32::new(difference_left.im * 0.5, -difference_left.re * 0.5) * derivative_scale;
            let derivative_right =
                Complex32::new(difference_right.im * 0.5, -difference_right.re * 0.5) * derivative_scale;
            bin.left_power = hann_left.norm_sqr();
            bin.right_power = hann_right.norm_sqr();
            bin.cross_power = (hann_left * hann_right.conj()).re;
            let power = bin.left_power + bin.right_power;
            let frequency = center as f64 * self.bin_width;
            let shift = (derivative_left * hann_left.conj()).im + (derivative_right * hann_right.conj()).im;
            let correction = radians_to_hertz * f64::from(shift / power);
            let reassigned = if power > 0.0 && correction.abs() <= lobe_half_width {
                frequency - correction
            } else {
                frequency
            };
            bin.reassigned_frequency = reassigned.clamp(MIN_FREQUENCY, MAX_FREQUENCY) as f32;
        }
        points.clear();
        for bin in &self.bins {
            let power = (bin.left_power + bin.right_power) * self.level_scale;
            if !power.is_finite() || power < SILENCE_POWER {
                continue;
            }
            let mut left_power = 0.0;
            let mut right_power = 0.0;
            let mut cross_power = 0.0;
            for neighbor in self
                .bins
                .get(bin.first_neighbor..=bin.last_neighbor)
                .unwrap_or_default()
            {
                left_power += neighbor.left_power;
                right_power += neighbor.right_power;
                cross_power += neighbor.cross_power;
            }
            let product = left_power * right_power;
            if ![left_power, right_power, cross_power, product]
                .into_iter()
                .all(f32::is_finite)
            {
                continue;
            }
            let coherence = if product > 0.0 {
                cross_power / product.sqrt()
            } else {
                1.0
            };
            if let Some(point) = SpectralPoint::new(
                bin.frequency,
                bin.reassigned_frequency,
                self.bin_width as f32,
                curve.position(left_power, right_power),
                power,
                coherence.clamp(-1.0, 1.0),
            ) {
                points.push(point);
            }
        }
    }
}

/// Separates real left and right transforms from one packed complex FFT using conjugate symmetry.
fn separate_channels(spectrum: &[Complex32], bin: usize) -> (Complex32, Complex32) {
    let packed = spectrum.get(bin).copied().unwrap_or_default();
    let mirrored = spectrum
        .get((spectrum.len() - bin) & (spectrum.len() - 1))
        .copied()
        .unwrap_or_default()
        .conj();
    let difference = (packed - mirrored) * 0.5;
    ((packed + mirrored) * 0.5, Complex32::new(difference.im, -difference.re))
}
