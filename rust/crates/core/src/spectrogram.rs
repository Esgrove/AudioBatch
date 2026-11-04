//! Bounded source-rate mel spectrograms and their portable shared cache representation.
//!
//! Centered symmetric-Hann windows retain independent mono or stereo channel powers.
//! Quantized maxima are merged in place, bounding the image to two MiB regardless of source duration.
//! The explicit 72-byte `SPEC` header pins every DSP profile field before a payload is allocated.

#![expect(
    clippy::suboptimal_flops,
    reason = "separate operations preserve the shared Hann, power normalization, and quantization rounding"
)]

use std::f64::consts::PI;
use std::sync::{Arc, OnceLock};

use realfft::num_complex::Complex32;
use realfft::{RealFftPlanner, RealToComplex};

/// Number of source samples in each real FFT window.
pub const FFT_SIZE: usize = 4096;
/// Source-frame distance between adjacent centered windows.
pub const HOP_SIZE: usize = 1024;
/// Equal-width HTK mel intervals between zero and source Nyquist.
pub const FREQUENCY_ROWS: usize = 512;
/// Hard bound on stored time columns, independent of source duration.
pub const MAXIMUM_COLUMNS: usize = 4096;
/// Revision of the window, power, frequency support, quantization, and time reduction contract.
pub const ALGORITHM_VERSION: u32 = 1;
/// Revision of the explicit little-endian cache envelope.
pub const SERIALIZATION_VERSION: u32 = 1;
/// Silence and smaller powers map to intensity zero.
pub const MINIMUM_DECIBELS: f64 = -120.0;
/// Powers at or above this ceiling map to intensity 255.
pub const MAXIMUM_DECIBELS: f64 = 24.0;

const HALF_WINDOW: usize = FFT_SIZE / 2;
const FREQUENCY_BINS: usize = HALF_WINDOW + 1;
const MAXIMUM_CHANNELS: usize = 2;
const MAXIMUM_SAMPLE_RATE: u32 = i32::MAX as u32;
const MAXIMUM_LENGTH: u64 = i64::MAX as u64;
const MAXIMUM_PAYLOAD_BYTES: usize = MAXIMUM_COLUMNS * FREQUENCY_ROWS;
const HEADER_BYTES: usize = 72;
const SERIALIZATION_MAGIC: u32 = 0x4345_5053;
const MEL_SCALE: f64 = 2595.0;
const MEL_FREQUENCY: f64 = 700.0;
const DECIBEL_STEPS: f64 = 255.0;
const MINIMUM_POWER: f64 = 1.0e-12;
static WINDOW: OnceLock<SpectrogramWindow> = OnceLock::new();

/// Immutable, validated column-major mel power maxima, with low frequencies first in each column.
///
/// Column `k` combines hop centers in `[k * samples_per_column, (k + 1) * samples_per_column)`.
/// Centers begin at zero and stop strictly before the actual decoded source length.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SpectrogramData {
    sample_rate: u32,
    channels: u32,
    length_in_frames: u64,
    samples_per_column: u64,
    intensities: Vec<u8>,
}

/// Builds an optional spectrogram from consecutive planar PCM without retaining the complete source.
///
/// Unsupported geometry, malformed planes, nonfinite samples, and FFT overflow invalidate only this artifact.
/// Each instance owns its mutable FFT scratch and is intended for one decoding worker.
pub struct SpectrogramBuilder {
    state: Option<Box<SpectrogramState>>,
}

/// Inclusive half-bin cells overlapping one mel interval, including shared support for sub-bin rows.
#[derive(Clone, Copy)]
struct RowSupport {
    first_bin: usize,
    last_bin: usize,
}

/// Immutable float coefficients and their ascending double-precision sum.
struct SpectrogramWindow {
    coefficients: [f32; FFT_SIZE],
    sum: f64,
}

/// Fixed PCM and FFT storage plus one bounded intensity vector for a supported stream.
struct SpectrogramState {
    sample_rate: u32,
    channels: usize,
    length: u64,
    stride: u64,
    next_center: u64,
    intensities: Vec<u8>,
    transform: Arc<dyn RealToComplex<f32>>,
    pcm: Box<[[f32; FFT_SIZE]]>,
    powers: Box<[f64]>,
    row_supports: [RowSupport; FREQUENCY_ROWS],
    input: Vec<f32>,
    output: Vec<Complex32>,
    scratch: Vec<Complex32>,
    inverse_window_power: f64,
}

/// Allocation-free cursor for the fully checked fixed-size header.
struct ByteReader<'a> {
    bytes: &'a [u8],
}

impl SpectrogramData {
    /// Adopts bytes only when the mono/stereo stream has exact, nonempty canonical bounded geometry.
    /// The rate and length must fit the shared signed 32-bit and signed 64-bit source fields.
    #[must_use]
    pub fn new(
        sample_rate: u32,
        channels: u32,
        length: u64,
        samples_per_column: u64,
        intensities: Vec<u8>,
    ) -> Option<Self> {
        let columns = expected_columns(sample_rate, channels, length, samples_per_column)?;
        if intensities.len() != columns * FREQUENCY_ROWS {
            return None;
        }
        Some(Self {
            sample_rate,
            channels,
            length_in_frames: length,
            samples_per_column,
            intensities,
        })
    }

    /// Decodes a supported `SPEC` envelope, rejecting every incompatible field and extra or missing byte.
    /// Complete geometry validation happens before allocating the bounded intensity payload.
    #[must_use]
    pub fn from_bytes(bytes: &[u8]) -> Option<Self> {
        if !(HEADER_BYTES..=HEADER_BYTES + MAXIMUM_PAYLOAD_BYTES).contains(&bytes.len()) {
            return None;
        }
        let mut reader = ByteReader { bytes };
        for expected in [
            SERIALIZATION_MAGIC,
            SERIALIZATION_VERSION,
            ALGORITHM_VERSION,
            FFT_SIZE as u32,
            HOP_SIZE as u32,
            FREQUENCY_ROWS as u32,
            MAXIMUM_COLUMNS as u32,
        ] {
            if reader.u32()? != expected {
                return None;
            }
        }
        let sample_rate = reader.u32()?;
        let channels = reader.u32()?;
        let columns = usize::try_from(reader.u32()?).ok()?;
        let length = reader.u64()?;
        let stride = reader.u64()?;
        if reader.u64()? != MINIMUM_DECIBELS.to_bits()
            || reader.u64()? != MAXIMUM_DECIBELS.to_bits()
            || columns != expected_columns(sample_rate, channels, length, stride)?
            || reader.bytes.len() != columns * FREQUENCY_ROWS
        {
            return None;
        }
        Some(Self {
            sample_rate,
            channels,
            length_in_frames: length,
            samples_per_column: stride,
            intensities: reader.bytes.to_vec(),
        })
    }

    /// Returns the decoded source sample rate without resampling.
    #[must_use]
    pub const fn sample_rate(&self) -> u32 {
        self.sample_rate
    }

    /// Returns the actual mono or stereo source channel count.
    #[must_use]
    pub const fn channels(&self) -> u32 {
        self.channels
    }

    /// Returns the actual decoded frame count rather than an estimated container duration.
    #[must_use]
    pub const fn length_in_frames(&self) -> u64 {
        self.length_in_frames
    }

    /// Returns the smallest power-of-two multiple of the hop that bounds the image width.
    #[must_use]
    pub const fn samples_per_column(&self) -> u64 {
        self.samples_per_column
    }

    /// Returns the number of complete time columns in this validated artifact.
    #[must_use]
    pub const fn column_count(&self) -> usize {
        self.intensities.len() / FREQUENCY_ROWS
    }

    /// Borrows the column-major quantized powers, each column containing low-to-high mel rows.
    #[must_use]
    pub const fn intensities(&self) -> &[u8] {
        self.intensities.as_slice()
    }

    /// Checks actual decoded stream geometry independently of presentation duration or measurements.
    #[must_use]
    pub const fn matches_stream(&self, rate: u32, length: u64, channels: u32) -> bool {
        self.sample_rate == rate && self.length_in_frames == length && self.channels == channels
    }

    /// Serializes the validated artifact using explicit little-endian fields and unchanged intensity bytes.
    #[must_use]
    pub fn to_bytes(&self) -> Vec<u8> {
        let mut bytes = Vec::with_capacity(HEADER_BYTES + self.intensities.len());
        for value in [
            SERIALIZATION_MAGIC,
            SERIALIZATION_VERSION,
            ALGORITHM_VERSION,
            FFT_SIZE as u32,
            HOP_SIZE as u32,
            FREQUENCY_ROWS as u32,
            MAXIMUM_COLUMNS as u32,
            self.sample_rate,
            self.channels,
            self.column_count() as u32,
        ] {
            bytes.extend_from_slice(&value.to_le_bytes());
        }
        for value in [
            self.length_in_frames,
            self.samples_per_column,
            MINIMUM_DECIBELS.to_bits(),
            MAXIMUM_DECIBELS.to_bits(),
        ] {
            bytes.extend_from_slice(&value.to_le_bytes());
        }
        bytes.extend_from_slice(&self.intensities);
        bytes
    }
}

impl SpectrogramBuilder {
    /// Starts a source-rate builder for mono or stereo PCM.
    /// Unsupported channels or rates produce an inert builder without allocating PCM, FFT, or grid storage.
    #[must_use]
    pub fn new(channels: usize, rate: u32) -> Self {
        let valid = (1..=MAXIMUM_CHANNELS).contains(&channels) && (1..=MAXIMUM_SAMPLE_RATE).contains(&rate);
        Self {
            state: valid.then(|| Box::new(SpectrogramState::new(channels, rate))),
        }
    }

    /// Consumes consecutive, equally sized channel planes without changing the supplied samples.
    /// Empty planes are harmless when the channel count matches.
    /// Malformed geometry or nonfinite PCM permanently disables this optional artifact.
    pub fn push<P: AsRef<[f32]>>(&mut self, planes: &[P]) {
        let Some(state) = self.state.as_mut() else {
            return;
        };
        if state.push(planes).is_none() {
            self.state = None;
        }
    }

    /// Flushes only centers strictly before actual EOF, padding missing PCM with zero.
    /// Empty streams and invalidated builders have no artifact.
    #[must_use]
    pub fn finish(self) -> Option<Arc<SpectrogramData>> {
        let mut state = self.state?;
        while state.next_center < state.length {
            state.append_column()?;
        }
        SpectrogramData::new(
            state.sample_rate,
            state.channels as u32,
            state.length,
            state.stride,
            state.intensities,
        )
        .map(Arc::new)
    }
}

impl SpectrogramState {
    /// Calculates source-rate row support and allocates reusable, fixed-size FFT and PCM working storage.
    fn new(channels: usize, rate: u32) -> Self {
        let transform = RealFftPlanner::<f32>::new().plan_fft_forward(FFT_SIZE);
        let window_sum = spectrogram_window().sum;
        let maximum_mel = frequency_to_mel(f64::from(rate) * 0.5);
        let mel_step = maximum_mel / FREQUENCY_ROWS as f64;
        let frequency_to_bin = FFT_SIZE as f64 / f64::from(rate);
        let row_supports = std::array::from_fn(|row| {
            let low_bin = mel_to_frequency(row as f64 * mel_step) * frequency_to_bin;
            let high_bin = mel_to_frequency((row + 1) as f64 * mel_step) * frequency_to_bin;
            let first_bin = (low_bin - 0.5).ceil().clamp(0.0, HALF_WINDOW as f64) as usize;
            let last_bin = (high_bin + 0.5).floor().clamp(first_bin as f64, HALF_WINDOW as f64) as usize;
            RowSupport { first_bin, last_bin }
        });
        Self {
            sample_rate: rate,
            channels,
            length: 0,
            stride: HOP_SIZE as u64,
            next_center: 0,
            intensities: Vec::with_capacity(MAXIMUM_PAYLOAD_BYTES),
            input: transform.make_input_vec(),
            output: transform.make_output_vec(),
            scratch: transform.make_scratch_vec(),
            transform,
            pcm: vec![[0.0; FFT_SIZE]; MAXIMUM_CHANNELS].into_boxed_slice(),
            powers: vec![0.0; FREQUENCY_BINS].into_boxed_slice(),
            row_supports,
            inverse_window_power: 1.0 / (window_sum * window_sum * channels as f64),
        }
    }

    /// Adds checked source frames and schedules a centered window as soon as its right half is available.
    fn push<P: AsRef<[f32]>>(&mut self, planes: &[P]) -> Option<()> {
        let frames = planes.first()?.as_ref().len();
        if planes.len() != self.channels
            || planes.iter().any(|plane| plane.as_ref().len() != frames)
            || self.length.checked_add(frames as u64)? > MAXIMUM_LENGTH - FFT_SIZE as u64
        {
            return None;
        }
        for frame in 0..frames {
            let position = (self.length % FFT_SIZE as u64) as usize;
            for (plane, ring) in planes.iter().zip(self.pcm.iter_mut()) {
                let sample = *plane.as_ref().get(frame)?;
                if !sample.is_finite() {
                    return None;
                }
                *ring.get_mut(position)? = sample;
            }
            self.length += 1;
            if self.length == self.next_center + HALF_WINDOW as u64 {
                self.append_column()?;
            }
        }
        Some(())
    }

    /// Measures mean independent channel powers with real endpoint calibration and half-bin mel maxima.
    fn append_column(&mut self) -> Option<()> {
        self.powers.fill(0.0);
        let window = &spectrogram_window().coefficients;
        for ring in self.pcm.iter().take(self.channels) {
            self.input.fill(0.0);
            for (frame, (input, coefficient)) in self.input.iter_mut().zip(window).enumerate() {
                if let Some(source) = (self.next_center + frame as u64)
                    .checked_sub(HALF_WINDOW as u64)
                    .filter(|&source| source < self.length)
                {
                    *input = ring.get((source % FFT_SIZE as u64) as usize)? * coefficient;
                }
            }
            self.transform
                .process_with_scratch(&mut self.input, &mut self.output, &mut self.scratch)
                .ok()?;
            for (bin, (power, value)) in self.powers.iter_mut().zip(&self.output).enumerate() {
                let real = f64::from(value.re);
                let imaginary = f64::from(value.im);
                let factor = if bin == 0 || bin == HALF_WINDOW { 1.0 } else { 4.0 };
                *power += (real * real + imaginary * imaginary) * factor * self.inverse_window_power;
            }
        }
        // Finite PCM can overflow a float FFT, which invalidates only this optional artifact.
        if self.powers.iter().any(|power| !power.is_finite()) {
            return None;
        }
        if self.next_center / self.stride >= MAXIMUM_COLUMNS as u64 {
            self.merge_pairs()?;
        }
        let column = usize::try_from(self.next_center / self.stride).ok()?;
        if column == self.intensities.len() / FREQUENCY_ROWS {
            self.intensities.resize(self.intensities.len() + FREQUENCY_ROWS, 0);
        }
        let destination = self
            .intensities
            .get_mut(column * FREQUENCY_ROWS..(column + 1) * FREQUENCY_ROWS)?;
        for (intensity, support) in destination.iter_mut().zip(&self.row_supports) {
            let maximum_power = self
                .powers
                .get(support.first_bin..=support.last_bin)?
                .iter()
                .copied()
                .fold(0.0, f64::max);
            *intensity = (*intensity).max(quantize_power(maximum_power));
        }
        self.next_center += HOP_SIZE as u64;
        Some(())
    }

    /// Reduces adjacent byte columns in place, preserving isolated maxima without another grid allocation.
    fn merge_pairs(&mut self) -> Option<()> {
        let merged_bytes = self.intensities.len() / 2;
        for destination in 0..merged_bytes {
            let column = destination / FREQUENCY_ROWS;
            let row = destination % FREQUENCY_ROWS;
            let source = 2 * column * FREQUENCY_ROWS + row;
            let maximum = (*self.intensities.get(source)?).max(*self.intensities.get(source + FREQUENCY_ROWS)?);
            *self.intensities.get_mut(destination)? = maximum;
        }
        self.intensities.truncate(merged_bytes);
        self.stride = self.stride.checked_mul(2)?;
        Some(())
    }
}

impl ByteReader<'_> {
    /// Advances over one explicitly little-endian 32-bit field.
    fn u32(&mut self) -> Option<u32> {
        let (field, remaining) = self.bytes.split_first_chunk::<4>()?;
        self.bytes = remaining;
        Some(u32::from_le_bytes(*field))
    }

    /// Advances over one explicitly little-endian 64-bit field.
    fn u64(&mut self) -> Option<u64> {
        let (field, remaining) = self.bytes.split_first_chunk::<8>()?;
        self.bytes = remaining;
        Some(u64::from_le_bytes(*field))
    }
}

/// Converts hertz to HTK mel, with zero hertz at zero mel.
#[must_use]
pub fn frequency_to_mel(frequency: f64) -> f64 {
    MEL_SCALE * (1.0 + frequency / MEL_FREQUENCY).log10()
}

/// Inverts the HTK mel definition used for every spectrogram row boundary.
#[must_use]
pub fn mel_to_frequency(mel: f64) -> f64 {
    MEL_FREQUENCY * (10.0_f64.powf(mel / MEL_SCALE) - 1.0)
}

/// Decodes an intensity to the center of its fixed linear decibel quantization level.
#[must_use]
pub fn spectrogram_decibels(intensity: u8) -> f64 {
    MINIMUM_DECIBELS + f64::from(intensity) * (MAXIMUM_DECIBELS - MINIMUM_DECIBELS) / DECIBEL_STEPS
}

/// Initializes symmetric Hann coefficients once, casting each double calculation before summing stored floats.
fn spectrogram_window() -> &'static SpectrogramWindow {
    WINDOW.get_or_init(|| {
        let coefficients =
            std::array::from_fn(|frame| (0.5 - 0.5 * (2.0 * PI * frame as f64 / (FFT_SIZE - 1) as f64).cos()) as f32);
        let mut sum = 0.0;
        for &coefficient in &coefficients {
            sum += f64::from(coefficient);
        }
        SpectrogramWindow { coefficients, sum }
    })
}

/// Clips calibrated power and rounds to the nearest intensity, with ties toward the larger byte.
fn quantize_power(power: f64) -> u8 {
    let decibels = 10.0 * power.max(MINIMUM_POWER).log10();
    let normalized =
        (decibels.clamp(MINIMUM_DECIBELS, MAXIMUM_DECIBELS) - MINIMUM_DECIBELS) / (MAXIMUM_DECIBELS - MINIMUM_DECIBELS);
    (normalized * DECIBEL_STEPS + 0.5).floor() as u8
}

/// Validates signed shared source fields and the smallest stride that bounds the complete image.
fn expected_columns(sample_rate: u32, channels: u32, length: u64, stride: u64) -> Option<usize> {
    if !(1..=MAXIMUM_SAMPLE_RATE).contains(&sample_rate)
        || !(1..=MAXIMUM_CHANNELS as u32).contains(&channels)
        || !(1..=MAXIMUM_LENGTH).contains(&length)
    {
        return None;
    }
    let mut canonical_stride = HOP_SIZE as u64;
    while length.div_ceil(canonical_stride) > MAXIMUM_COLUMNS as u64 {
        canonical_stride *= 2;
    }
    if stride != canonical_stride {
        return None;
    }
    usize::try_from(length.div_ceil(canonical_stride)).ok()
}

#[cfg(test)]
mod test_reduction {
    use super::*;

    /// In-place reduction must preserve each row's independent maximum through consecutive merges.
    #[test]
    fn adjacent_pair_maxima_preserve_every_row_without_reallocating() {
        let mut state = SpectrogramState::new(1, 48_000);
        state.intensities.extend((0..MAXIMUM_PAYLOAD_BYTES).map(|index| {
            let column = index / FREQUENCY_ROWS;
            let row = index % FREQUENCY_ROWS;
            ((column * 17 + row * 29) % 256) as u8
        }));
        let original = state.intensities.clone();
        let allocation = state.intensities.as_ptr();
        for merge in 1..=3 {
            state.merge_pairs().expect("bounded merge");
            let group_size = 1 << merge;
            assert_eq!(state.intensities.len(), MAXIMUM_PAYLOAD_BYTES / group_size);
            assert_eq!(state.intensities.capacity(), MAXIMUM_PAYLOAD_BYTES);
            assert_eq!(state.intensities.as_ptr(), allocation);
            assert_eq!(state.stride, (HOP_SIZE * group_size) as u64);
            for (destination, &actual) in state.intensities.iter().enumerate() {
                let column = destination / FREQUENCY_ROWS;
                let row = destination % FREQUENCY_ROWS;
                let expected = (0..group_size)
                    .map(|offset| {
                        *original
                            .get((column * group_size + offset) * FREQUENCY_ROWS + row)
                            .expect("source row")
                    })
                    .max()
                    .expect("nonempty reduction");
                assert_eq!(actual, expected);
            }
        }
    }

    /// The explicit stored-float Hann contract fixes symmetry, endpoint zeros, and ascending summation.
    #[test]
    fn shared_hann_coefficients_use_the_explicit_double_formula() {
        let window = spectrogram_window();
        let mut expected_sum = 0.0;
        for (frame, &coefficient) in window.coefficients.iter().enumerate() {
            let expected = (0.5 - 0.5 * (2.0 * PI * frame as f64 / 4095.0).cos()) as f32;
            assert_eq!(coefficient.to_bits(), expected.to_bits());
            expected_sum += f64::from(expected);
        }
        assert_eq!(window.sum.to_bits(), expected_sum.to_bits());
        assert!(
            window
                .coefficients
                .first()
                .is_some_and(|value| value.abs() < f32::EPSILON)
        );
        assert!(
            window
                .coefficients
                .last()
                .is_some_and(|value| value.abs() < f32::EPSILON)
        );
        assert!(std::ptr::eq(window, spectrogram_window()));
    }

    /// Quantization is monotone, allowing byte-domain maxima without changing the physical reduction contract.
    #[test]
    fn power_quantization_has_fixed_endpoints_and_monotone_rounding() {
        assert_eq!(quantize_power(0.0), 0);
        assert_eq!(quantize_power(1.0e-12), 0);
        assert_eq!(quantize_power(1.0), 213);
        assert_eq!(quantize_power(10.0_f64.powf(2.4)), 255);
        assert_eq!(quantize_power(f64::MAX), 255);
        let mut previous = 0;
        for step in 0..=14_400 {
            let decibels = -120.0 + f64::from(step) * 0.01;
            let power = 10.0_f64.powf(decibels / 10.0);
            let actual = quantize_power(power);
            assert!(actual >= previous);
            previous = actual;
        }
    }

    /// The source-rate support is inclusive and shares a nearest FFT cell for narrow low-frequency rows.
    #[test]
    fn mel_support_covers_endpoints_and_never_has_empty_rows() {
        for rate in [1, 44_100, 48_000, 96_000, i32::MAX as u32] {
            let state = SpectrogramState::new(2, rate);
            assert_eq!(state.pcm.len(), MAXIMUM_CHANNELS);
            assert_eq!(state.powers.len(), FREQUENCY_BINS);
            assert_eq!(state.row_supports.first().expect("lowest row").first_bin, 0);
            assert_eq!(state.row_supports.last().expect("highest row").last_bin, HALF_WINDOW);
            for support in state.row_supports {
                assert!(support.first_bin <= support.last_bin);
                assert!(support.last_bin <= HALF_WINDOW);
            }
        }
    }
}
