//! Streaming planar true peak measurement with four interpolation phases in SIMD lanes.
//!
//! The interpolator uses the coefficients and tap order of ebur128 0.1.11 without fused multiplication and addition.
//! It retains its history between calls and includes decoded sample peaks without flushing an interpolation tail.
//! Both cumulative peaks and the peaks produced by the last nonempty push are available without allocation.
//! NaNs do not replace a peak, and infinite decoded samples produce infinite peaks.
//! Hardware execution targets Apple Silicon NEON and x86 AVX2, including AVX-512 capable AMD CPUs.
//! The x86 path selects AVX2 even when wider instructions are available, without SSE compatibility backends.
//! Scalar fallback remains available for differential tests.
//!
//! On x86, arithmetic results are flushed to zero to match ebur128, without discarding subnormal inputs.
//! The kernel emulates that behavior without changing the caller's floating point environment.
//! Other targets retain their native arithmetic-underflow behavior.

use std::f64::consts::PI;
use std::sync::LazyLock;

use fearless_simd::{Level, Simd, f32x4, f64x4, prelude::*};
use fearless_simd_macros::simd;

/// NEON or AVX2 hardware level, detected once per process.
static LEVEL: LazyLock<Level> = LazyLock::new(hardware_level);

/// Native four-phase coefficients and their underflow bound, shared without sharing channel history.
static FOUR_FILTER: LazyLock<Filter<12>> = LazyLock::new(|| Filter::new(interpolation_coefficients::<12, 4>()));

/// Native two-phase coefficients duplicated into four lanes, with their shared underflow bound.
static TWO_FILTER: LazyLock<Filter<24>> = LazyLock::new(|| {
    Filter::new(interpolation_coefficients::<24, 2>().map(|[first, second]| [first, second, first, second]))
});

/// Total coefficients in the reference interpolation filter and storage for a doubled history.
const FILTER_TAPS: usize = 48;

/// Distance from zero below which the coefficient generator uses the sinc limit.
const ALMOST_ZERO: f64 = 0.000_001;

/// Smallest product, 2^-102, above which the spacing is at least 2^-126,
/// so sums of such products cannot become subnormal.
#[cfg(any(target_arch = "x86", target_arch = "x86_64"))]
const MINIMUM_PRODUCT: f64 = f32::MIN_POSITIVE as f64 * (1_u32 << f32::MANTISSA_DIGITS) as f64;

/// Returns the smaller of `minimum` and the magnitude of `value`, ignoring zeros.
#[cfg(any(target_arch = "x86", target_arch = "x86_64"))]
const fn smallest_nonzero_magnitude(minimum: f32, value: f32) -> f32 {
    let magnitude = value.abs();
    if magnitude > 0.0 {
        minimum.min(magnitude)
    } else {
        minimum
    }
}

/// Interpolation factor and its fixed size channel histories.
#[derive(Debug, Clone)]
enum Interpolation {
    /// Four phases below 96 kHz, with one phase per SIMD lane.
    Four {
        filter: &'static Filter<12>,
        histories: Vec<History<12>>,
    },
    /// Two phases below 192 kHz, duplicated into the upper two SIMD lanes.
    Two {
        filter: &'static Filter<24>,
        histories: Vec<History<24>>,
    },
    /// At 192 kHz and above only decoded sample peaks are needed.
    Samples,
}

/// Streaming per-channel true peaks in linear amplitude units.
///
/// Input is planar, with equally sized planes in the channel order passed to [`Self::new`].
/// Construction allocates channel histories and peak storage, but processing and reading peaks do not allocate.
#[doc(hidden)]
#[derive(Debug, Clone)]
pub struct TruePeak {
    /// Rate-specific interpolation state.
    interpolation: Interpolation,
    /// Running maxima including both decoded samples and interpolated samples.
    peaks: Vec<f64>,
    /// Maxima from the most recent nonempty push, including interpolation of retained history.
    previous_peaks: Vec<f64>,
}

/// Immutable interpolation data initialized with native math before processing begins.
#[derive(Debug)]
struct Filter<const TAPS: usize> {
    coefficients: [[f32; 4]; TAPS],
    minimum_fast_sample: f32,
}

/// Doubled circular buffer exposing the most recent samples as a fixed size array.
#[derive(Debug, Clone)]
struct History<const TAPS: usize> {
    /// Every sample is written twice so the current history is contiguous across a wrap.
    samples: [f32; FILTER_TAPS],
    /// Index of the newest sample after the first insertion.
    position: usize,
    /// Slots whose nonzero samples can produce subnormal products or sums.
    small_samples: u32,
}

impl TruePeak {
    /// Creates an empty meter for a fixed channel count and sample rate in hertz.
    ///
    /// # Panics
    ///
    /// Panics if the channel count or sample rate is zero.
    #[must_use]
    pub fn new(channels: usize, sample_rate: u32) -> Self {
        assert!(channels > 0, "true peak needs at least one channel");
        assert!(sample_rate > 0, "true peak needs a positive sample rate");
        let interpolation = if sample_rate < 96_000 {
            Interpolation::Four {
                filter: &FOUR_FILTER,
                histories: vec![History::new(); channels],
            }
        } else if sample_rate < 192_000 {
            Interpolation::Two {
                filter: &TWO_FILTER,
                histories: vec![History::new(); channels],
            }
        } else {
            Interpolation::Samples
        };
        Self {
            interpolation,
            peaks: vec![0.0; channels],
            previous_peaks: vec![0.0; channels],
        }
    }

    /// Processes a planar block without draining or resetting interpolation history.
    ///
    /// Empty blocks are allowed and do not change the peaks or the history.
    ///
    /// # Panics
    ///
    /// Panics if the CPU does not support NEON or AVX2, the number of planes differs from the configured channel count,
    /// or the planes have different lengths.
    pub fn push<P: AsRef<[f32]>>(&mut self, planes: &[P]) {
        self.push_at_level(planes, *LEVEL);
    }

    /// Returns the accumulated true peak of each channel in linear amplitude units.
    #[must_use]
    pub const fn peaks(&self) -> &[f64] {
        self.peaks.as_slice()
    }

    /// Returns each channel's true peak from the most recent nonempty push in linear amplitude units.
    ///
    /// Peaks start at positive zero and empty pushes leave them unchanged.
    /// Interpolated output includes retained history, without draining a tail at the end of a push.
    #[doc(hidden)]
    #[must_use]
    pub const fn previous_peaks(&self) -> &[f64] {
        self.previous_peaks.as_slice()
    }

    /// Rejects malformed planar input before any state is changed.
    fn validate_planes<P: AsRef<[f32]>>(&self, planes: &[P]) {
        assert_eq!(planes.len(), self.peaks.len(), "true peak channel count changed");
        let frames = planes.first().map_or(0, |plane| plane.as_ref().len());
        assert!(
            planes.iter().all(|plane| plane.as_ref().len() == frames),
            "true peak planes have different lengths"
        );
    }

    /// Processes one block with an explicit token, also allowing exact backend coverage in tests.
    #[simd]
    fn push_simd<S: Simd, P: AsRef<[f32]>>(&mut self, simd: S, planes: &[P]) {
        if planes.first().is_none_or(|plane| plane.as_ref().is_empty()) {
            return;
        }
        match &mut self.interpolation {
            Interpolation::Four { filter, histories } => push_interpolated(
                simd,
                planes,
                &mut self.peaks,
                &mut self.previous_peaks,
                filter,
                histories,
            ),
            Interpolation::Two { filter, histories } => push_interpolated(
                simd,
                planes,
                &mut self.peaks,
                &mut self.previous_peaks,
                filter,
                histories,
            ),
            Interpolation::Samples => {
                for ((plane, peak), previous_peak) in planes.iter().zip(&mut self.peaks).zip(&mut self.previous_peaks) {
                    let mut block_peak = 0.0;
                    for &sample in plane.as_ref() {
                        let magnitude = f64::from(sample).abs();
                        if magnitude > block_peak {
                            block_peak = magnitude;
                        }
                    }
                    *previous_peak = block_peak;
                    if block_peak > *peak {
                        *peak = block_peak;
                    }
                }
            }
        }
    }

    /// Runs a hardware backend directly, with scalar fallback available only for differential tests.
    fn push_at_level<P: AsRef<[f32]>>(&mut self, planes: &[P], level: Level) {
        self.validate_planes(planes);
        match level {
            #[cfg(any(target_arch = "x86", target_arch = "x86_64"))]
            Level::Avx2(simd) => self.push_simd(simd, planes),
            #[cfg(target_arch = "aarch64")]
            Level::Neon(simd) => self.push_simd(simd, planes),
            #[cfg(test)]
            Level::Fallback(simd) => self.push_simd(simd, planes),
            _ => unreachable!("true peak requires NEON or AVX2"),
        }
    }
}

impl<const TAPS: usize> Filter<TAPS> {
    /// Derives the conservative x86 arithmetic-underflow bound once for a fixed coefficient bank.
    const fn new(coefficients: [[f32; 4]; TAPS]) -> Self {
        #[cfg(any(target_arch = "x86", target_arch = "x86_64"))]
        let minimum_fast_sample = {
            // Iterators are not usable in const fn, so walk the rows with slice patterns.
            let mut minimum_coefficient = f32::INFINITY;
            let mut rows = coefficients.as_slice();
            while let [[first, second, third, fourth], rest @ ..] = rows {
                minimum_coefficient = smallest_nonzero_magnitude(minimum_coefficient, *first);
                minimum_coefficient = smallest_nonzero_magnitude(minimum_coefficient, *second);
                minimum_coefficient = smallest_nonzero_magnitude(minimum_coefficient, *third);
                minimum_coefficient = smallest_nonzero_magnitude(minimum_coefficient, *fourth);
                rows = rest;
            }
            (MINIMUM_PRODUCT / minimum_coefficient as f64) as f32
        }
        .next_up();
        #[cfg(not(any(target_arch = "x86", target_arch = "x86_64")))]
        let minimum_fast_sample = 0.0;
        Self {
            coefficients,
            minimum_fast_sample,
        }
    }
}

impl<const TAPS: usize> History<TAPS> {
    /// Creates an all-zero interpolation history whose doubled storage fits the fixed buffer.
    const fn new() -> Self {
        assert!(TAPS > 0 && TAPS * 2 <= FILTER_TAPS);
        Self {
            samples: [0.0; FILTER_TAPS],
            position: TAPS,
            small_samples: 0,
        }
    }

    /// Inserts a sample and returns the newest-first history and whether underflow needs explicit handling.
    #[expect(
        clippy::inline_always,
        reason = "the fixed history view must inline into the SIMD kernel"
    )]
    #[expect(
        clippy::indexing_slicing,
        reason = "the position stays below TAPS and the buffer holds two histories"
    )]
    #[inline(always)]
    fn push_front(&mut self, sample: f32, minimum_fast_sample: f32) -> (&[f32; TAPS], bool) {
        self.position = if self.position == 0 {
            TAPS - 1
        } else {
            self.position - 1
        };
        let slot = 1_u32 << self.position;
        self.small_samples &= !slot;
        // Clear the sign bit so unsigned comparisons order magnitudes without floating-point denormal handling.
        let magnitude = sample.to_bits() & 0x7fff_ffff;
        if magnitude < minimum_fast_sample.to_bits() && magnitude != 0 {
            self.small_samples |= slot;
        }
        self.samples[self.position] = sample;
        self.samples[self.position + TAPS] = sample;
        (
            self.samples[self.position..]
                .first_chunk::<TAPS>()
                .expect("the doubled buffer always contains a complete history"),
            self.small_samples != 0,
        )
    }
}

/// Detects the supported hardware baseline, using AVX2 even on AVX-512 capable x86 CPUs.
fn hardware_level() -> Level {
    let detected = Level::new();
    #[cfg(any(target_arch = "x86", target_arch = "x86_64"))]
    {
        Level::Avx2(
            detected
                .as_avx2()
                .expect("audio analysis requires an AVX2 capable x86 CPU"),
        )
    }
    #[cfg(target_arch = "aarch64")]
    {
        Level::Neon(
            detected
                .as_neon()
                .expect("audio analysis requires an ARM CPU with NEON"),
        )
    }
    #[cfg(not(any(target_arch = "x86", target_arch = "x86_64", target_arch = "aarch64")))]
    {
        let _ = detected;
        unreachable!("audio analysis targets Apple Silicon NEON and x86 AVX2")
    }
}

/// Processes all planes with reference tap order, updating previous and cumulative peaks from independent phase maxima.
#[simd]
fn push_interpolated<S: Simd, P: AsRef<[f32]>, const TAPS: usize>(
    simd: S,
    planes: &[P],
    peaks: &mut [f64],
    previous_peaks: &mut [f64],
    filter: &Filter<TAPS>,
    histories: &mut [History<TAPS>],
) {
    let minimum_fast_sample = filter.minimum_fast_sample;
    let scalar_coefficients = &filter.coefficients;
    let coefficients = filter.coefficients.map(|row| f32x4::simd_from(simd, row));
    let zero = f32x4::splat(simd, 0.0);
    for (((plane, peak), previous_peak), history) in planes.iter().zip(peaks).zip(previous_peaks).zip(histories) {
        let mut block_peak = 0.0;
        let mut phase_peaks = zero;
        for &sample in plane.as_ref() {
            // Convert before comparing, as the reference sample peak scanner does.
            // This also keeps decoded subnormals representable when single precision comparisons flush inputs.
            let magnitude = f64::from(sample).abs();
            if magnitude > block_peak {
                block_peak = magnitude;
            }
            let (recent, underflow_possible) = history.push_front(sample, minimum_fast_sample);
            let output = if underflow_possible {
                interpolate_underflow(simd, recent, scalar_coefficients)
            } else {
                let mut output = zero;
                for (&coefficient, &input) in coefficients.iter().zip(recent) {
                    // Separate operators are intentional, the reference does not enable precision-true-peak.
                    let product = f32x4::splat(simd, input) * coefficient;
                    output += product;
                }
                output
            };
            let magnitudes = output.abs();
            // SIMD max has backend-dependent NaN semantics, while ordered comparison ignores every NaN.
            phase_peaks = magnitudes.simd_gt(phase_peaks).select(magnitudes, phase_peaks);
        }
        // Every lane is now a nonnegative, non-NaN maximum.
        // A scalar comparison still makes the required NaN policy explicit during reduction.
        for magnitude in <[f32; 4]>::from(phase_peaks) {
            let magnitude = f64::from(magnitude);
            if magnitude > block_peak {
                block_peak = magnitude;
            }
        }
        *previous_peak = block_peak;
        if block_peak > *peak {
            *peak = block_peak;
        }
    }
}

/// Emulates each binary32 multiply and add with FTZ, keeping intermediates normal in binary64.
#[simd]
fn interpolate_underflow<S: Simd, const TAPS: usize>(
    simd: S,
    inputs: &[f32; TAPS],
    coefficients: &[[f32; 4]; TAPS],
) -> f32x4<S> {
    let zero = f64x4::splat(simd, 0.0);
    let minimum = f64x4::splat(simd, f64::from(f32::MIN_POSITIVE));
    let round = |value: f64x4<S>| {
        let value = value.abs().simd_lt(minimum).select(zero, value);
        let narrowed = value.narrow(value).split().0;
        let (low, high) = narrowed.widen();
        low.combine(high)
    };
    let mut output = zero;
    for (&input, row) in inputs.iter().zip(coefficients) {
        let coefficient = f64x4::simd_from(simd, row.map(f64::from));
        let product = round(f64x4::splat(simd, f64::from(input)) * coefficient);
        output = round(output + product);
    }
    output.narrow(output).split().0
}

// Interpolation coefficient calculation is adapted from ebur128 0.1.11.
// The upstream copyright and MIT license notice are in ../../licenses/ebur128.txt.

/// Calculates the reference Hanning-windowed sinc coefficients with the original f64 evaluation order.
fn interpolation_coefficients<const TAPS: usize, const FACTOR: usize>() -> [[f32; FACTOR]; TAPS] {
    assert_eq!(TAPS * FACTOR, FILTER_TAPS);
    let mut filter = [[0.0; FACTOR]; TAPS];
    let window = FILTER_TAPS as f64;
    let half_window = window * 0.5;
    for (index, coefficient) in filter.iter_mut().flat_map(|row| row.iter_mut()).enumerate() {
        let index = index as f64;
        let weight = 0.5 * (1.0 - f64::cos(2.0 * PI * index / window));
        let offset = index - half_window;
        *coefficient = if offset.abs() > ALMOST_ZERO {
            let argument = offset * PI / FACTOR as f64;
            weight * f64::sin(argument) / argument
        } else {
            weight
        } as f32;
    }
    filter
}

#[cfg(test)]
mod test_true_peak {
    use ebur128::{EbuR128, Mode};

    use super::*;
    use crate::signals::Xorshift;

    #[test]
    fn shared_filters_retain_every_native_coefficient_bit() {
        for (actual, expected) in FOUR_FILTER
            .coefficients
            .iter()
            .flatten()
            .zip(interpolation_coefficients::<12, 4>().into_iter().flatten())
            .chain(
                TWO_FILTER.coefficients.iter().flatten().zip(
                    interpolation_coefficients::<24, 2>()
                        .map(|[first, second]| [first, second, first, second])
                        .into_iter()
                        .flatten(),
                ),
            )
        {
            assert_eq!(actual.to_bits(), expected.to_bits());
        }
        for rate in [48_000, 96_000] {
            let first = TruePeak::new(1, rate);
            let second = TruePeak::new(2, rate);
            match (&first.interpolation, &second.interpolation) {
                (Interpolation::Four { filter: first, .. }, Interpolation::Four { filter: second, .. }) => {
                    assert!(std::ptr::eq(*first, *second));
                }
                (Interpolation::Two { filter: first, .. }, Interpolation::Two { filter: second, .. }) => {
                    assert!(std::ptr::eq(*first, *second));
                }
                _ => panic!("the same rate must select the same interpolation"),
            }
        }
    }

    /// Returns the hardware backend and the dependency's forced scalar fallback for exact comparisons.
    fn supported_levels() -> [Level; 2] {
        [Level::fallback(), hardware_level()]
    }

    /// Creates a short repeatable noise signal with a distinct history in every channel.
    fn seeded_planes(channels: usize, frames: usize) -> Vec<Vec<f32>> {
        let mut random = Xorshift::new(0xA103_96B7);
        (0..channels)
            .map(|_| (0..frames).map(|_| random.next_signed() as f32 * 0.75).collect())
            .collect()
    }

    /// Creates small, irregular chunks so the rolling history wraps on both sides of block boundaries.
    fn random_chunks() -> Vec<usize> {
        let mut random = Xorshift::new(0x53B1_24F9);
        let mut chunks = vec![0, 1, 0];
        chunks.extend((0..19).map(|_| 1 + random.next_below(37) as usize));
        chunks
    }

    /// Compares cumulative and previous peaks bit for bit, including an empty push after every chunk.
    fn assert_reference(planes: &[Vec<f32>], sample_rate: u32, chunk_sizes: &[usize]) {
        let channels = planes.len();
        let frames = planes.first().map_or(0, Vec::len);
        let mut reference = EbuR128::new(channels as u32, sample_rate, Mode::TRUE_PEAK).expect("reference meter");
        let mut candidates: Vec<_> = supported_levels()
            .into_iter()
            .map(|level| (level, TruePeak::new(channels, sample_rate)))
            .collect();
        let mut start = 0;
        let mut chunk_index = 0;
        loop {
            let count = chunk_sizes[chunk_index % chunk_sizes.len()];
            let end = (start + count).min(frames);
            for empty in [false, true] {
                let chunk_start = if empty { end } else { start };
                let chunk: Vec<_> = planes.iter().map(|plane| &plane[chunk_start..end]).collect();
                reference.add_frames_planar_f32(&chunk).expect("reference block");
                for (level, candidate) in &mut candidates {
                    candidate.push_at_level(&chunk, *level);
                    for (channel, (&cumulative, &previous)) in
                        candidate.peaks().iter().zip(candidate.previous_peaks()).enumerate()
                    {
                        let expected_cumulative = reference.true_peak(channel as u32).expect("reference true peak");
                        let expected_previous = reference
                            .prev_true_peak(channel as u32)
                            .expect("reference previous true peak");
                        for (kind, actual, expected) in [
                            ("cumulative", cumulative, expected_cumulative),
                            ("previous", previous, expected_previous),
                        ] {
                            assert_eq!(
                                actual.to_bits(),
                                expected.to_bits(),
                                "{kind} peak, rate {sample_rate}, channels {channels}, channel {channel}, \
                                 frames {chunk_start}..{end}, level {level:?}, actual {actual:?}, expected {expected:?}"
                            );
                        }
                    }
                }
            }
            start = end;
            chunk_index += 1;
            if start == frames {
                break;
            }
        }
    }

    #[test]
    fn seeded_four_phase_signals_match_at_every_level() {
        let planes = seeded_planes(2, 257);
        for rate in [8000, 44_100, 48_000, 88_200, 95_999] {
            assert_reference(&planes, rate, &random_chunks());
        }
    }

    #[test]
    fn seeded_two_phase_signals_match_at_every_level() {
        let planes = seeded_planes(2, 257);
        for rate in [96_000, 96_001, 176_400, 191_999] {
            assert_reference(&planes, rate, &random_chunks());
        }
    }

    #[test]
    fn sample_only_rates_match_at_every_level() {
        let planes = seeded_planes(3, 129);
        for rate in [192_000, 192_001, 384_000] {
            assert_reference(&planes, rate, &random_chunks());
        }
    }

    #[test]
    fn specialized_and_generic_channel_counts_match() {
        for channels in [1, 3, 4, 6, 8, 9] {
            let planes = seeded_planes(channels, 97);
            for rate in [48_000, 96_000] {
                assert_reference(&planes, rate, &[1, 12, 24, 7]);
            }
        }
    }

    #[test]
    fn previous_peaks_drop_after_silence_drains_history() {
        let mut planes = vec![vec![0.0; 74]];
        planes[0][0] = 1.0;
        planes[0][49] = -0.25;
        for rate in [48_000, 96_000, 192_000] {
            assert_reference(&planes, rate, &[1, 24, 24, 1, 24]);
            for level in supported_levels() {
                let mut candidate = TruePeak::new(1, rate);
                let previous_storage = candidate.previous_peaks().as_ptr();
                candidate.push_at_level(&[[1.0]], level);
                assert_eq!(candidate.previous_peaks()[0].to_bits(), 1.0_f64.to_bits());
                candidate.push_at_level(&[[0.0; 24]], level);
                if rate < 192_000 {
                    assert!(
                        candidate.previous_peaks()[0] > 0.0,
                        "retained history must contribute at {level:?}"
                    );
                }
                candidate.push_at_level(&[[0.0; 24]], level);
                assert_eq!(candidate.previous_peaks()[0].to_bits(), 0.0_f64.to_bits());
                candidate.push_at_level(&[[-0.25]], level);
                candidate.push_at_level(&[[]], level);
                assert_eq!(candidate.previous_peaks()[0].to_bits(), 0.25_f64.to_bits());
                assert_eq!(candidate.peaks()[0].to_bits(), 1.0_f64.to_bits());
                assert_eq!(candidate.previous_peaks().as_ptr(), previous_storage);
            }
        }
    }

    #[test]
    fn full_scale_neighbors_remain_channel_local() {
        let below = f32::from_bits(1.0_f32.to_bits() - 1);
        let above = f32::from_bits(1.0_f32.to_bits() + 1);
        let first = [below, -1.0, above, 0.0];
        let second = [-above, below, 0.0, 1.0];
        let planes: Vec<_> = first
            .iter()
            .zip(second)
            .map(|(&initial, later)| {
                let mut plane = vec![0.0; 74];
                plane[0] = initial;
                plane[49] = later;
                plane
            })
            .collect();
        for rate in [48_000, 96_000, 192_000] {
            assert_reference(&planes, rate, &[0, 1, 24, 24, 0, 1, 24]);
            for level in supported_levels() {
                let mut candidate = TruePeak::new(4, rate);
                candidate.push_at_level(&first.map(|sample| [sample]), level);
                for (&actual, sample) in candidate.previous_peaks().iter().zip(first) {
                    assert_eq!(actual.to_bits(), f64::from(sample).abs().to_bits());
                }
                candidate.push_at_level(&[[0.0; 24]; 4], level);
                candidate.push_at_level(&[[0.0; 24]; 4], level);
                candidate.push_at_level(&second.map(|sample| [sample]), level);
                for ((&previous, &cumulative), (initial, later)) in candidate
                    .previous_peaks()
                    .iter()
                    .zip(candidate.peaks())
                    .zip(first.into_iter().zip(second))
                {
                    assert_eq!(previous.to_bits(), f64::from(later).abs().to_bits());
                    assert_eq!(
                        cumulative.to_bits(),
                        f64::from(initial.abs().max(later.abs())).to_bits()
                    );
                }
            }
        }
    }

    #[test]
    fn intersample_overshoots_remain_channel_local() {
        let planes: Vec<_> = [0.75, 0.25, 0.0]
            .into_iter()
            .map(|amplitude| {
                (0..97)
                    .map(|frame| if frame % 4 < 2 { amplitude } else { -amplitude })
                    .collect()
            })
            .collect();
        for rate in [48_000, 96_000] {
            assert_reference(&planes, rate, &[1, 12, 0, 24, 7]);
            for level in supported_levels() {
                let mut candidate = TruePeak::new(3, rate);
                candidate.push_at_level(&planes, level);
                assert!(candidate.previous_peaks()[0] > 1.0);
                assert!(candidate.previous_peaks()[1] > 0.25);
                assert!(candidate.previous_peaks()[1] < 1.0);
                assert_eq!(candidate.previous_peaks()[2].to_bits(), 0.0_f64.to_bits());
            }
        }
    }

    #[test]
    fn nan_histories_do_not_replace_finite_peaks() {
        let mut planes = vec![vec![0.0; 73]; 3];
        for (channel, plane) in planes.iter_mut().enumerate() {
            plane[0] = 0.125;
            plane[1 + channel] = f32::from_bits(0x7FC0_1234);
            plane[9 + channel] = f32::from_bits(0xFFC0_4321);
            plane[37] = -0.25;
            plane[38] = -0.25;
            plane[39] = 0.25;
            plane[40] = 0.25;
        }
        for rate in [48_000, 96_000, 192_000] {
            assert_reference(&planes, rate, &[1]);
        }
        for rate in [48_000, 96_000, 192_000] {
            assert_reference(&[vec![f32::NAN; 29]], rate, &[0, 1, 7]);
        }
    }

    #[test]
    fn infinite_sample_peaks_survive_nan_interpolation() {
        let planes = vec![
            vec![0.0, f32::NAN, f32::INFINITY, -0.0, 0.75],
            vec![-0.0, f32::NEG_INFINITY, f32::NAN, f32::INFINITY, 0.0],
        ];
        for rate in [48_000, 96_000, 192_000] {
            assert_reference(&planes, rate, &[1]);
        }
    }

    #[test]
    fn zeros_and_empty_blocks_keep_positive_zero_peaks() {
        for rate in [48_000, 96_000, 192_000] {
            assert_reference(&[vec![], vec![]], rate, &[0]);
            assert_reference(&[vec![0.0; 49], vec![-0.0; 49]], rate, &[0, 1, 12]);
        }
    }

    #[test]
    fn short_inputs_include_raw_peaks_without_flushing_a_tail() {
        for frames in [1, 2, 3, 11, 12, 13, 23, 24, 25] {
            let planes = seeded_planes(1, frames);
            for rate in [48_000, 96_000, 192_000] {
                assert_reference(&planes, rate, &[frames]);
            }
        }
        for rate in [48_000, 96_000] {
            let mut candidate = TruePeak::new(1, rate);
            candidate.push(&[[0.5, 0.5, -0.5, -0.5]]);
            let before = candidate.peaks()[0].to_bits();
            let previous_before = candidate.previous_peaks()[0].to_bits();
            candidate.push(&[[]]);
            assert_eq!(candidate.peaks()[0].to_bits(), before);
            assert_eq!(candidate.previous_peaks()[0].to_bits(), previous_before);
            assert_eq!(before, 0.5_f64.to_bits());
            assert_eq!(previous_before, 0.5_f64.to_bits());
        }
    }

    #[test]
    fn public_runtime_dispatch_matches_forced_levels() {
        let planes = seeded_planes(2, 73);
        for rate in [48_000, 96_000, 192_000] {
            let mut runtime = TruePeak::new(2, rate);
            runtime.push(&planes);
            for level in supported_levels() {
                let mut forced = TruePeak::new(2, rate);
                forced.push_at_level(&planes, level);
                for (actual, expected) in [
                    (runtime.peaks(), forced.peaks()),
                    (runtime.previous_peaks(), forced.previous_peaks()),
                ] {
                    for (&actual, &expected) in actual.iter().zip(expected) {
                        assert_eq!(actual.to_bits(), expected.to_bits(), "{level:?}");
                    }
                }
            }
        }
    }

    #[test]
    fn decoded_subnormal_peaks_match_without_interpolation() {
        let planes = vec![vec![f32::from_bits(1), -f32::from_bits(0x007F_FFFF), 0.0]];
        assert_reference(&planes, 192_000, &[1]);
    }

    #[test]
    fn normal_inputs_with_underflowing_products_match_reference() {
        let planes: Vec<_> = seeded_planes(2, 129)
            .into_iter()
            .map(|plane| {
                plane
                    .into_iter()
                    .map(|sample| (1.0 + sample.abs()).copysign(sample) * f32::MIN_POSITIVE)
                    .collect::<Vec<_>>()
            })
            .collect();
        assert!(planes.iter().flatten().all(|sample| sample.is_normal()));
        for rate in [22_050, 48_000, 96_000, 192_000] {
            assert_reference(&planes, rate, &random_chunks());
        }
    }

    #[test]
    fn subnormal_inputs_match_reference() {
        let amplitude = f32::MIN_POSITIVE * 0.5;
        let planes = vec![
            (0..65)
                .map(|frame| if frame % 4 < 2 { amplitude } else { -amplitude })
                .collect::<Vec<_>>(),
        ];
        for rate in [22_050, 48_000, 96_000, 192_000] {
            assert_reference(&planes, rate, &random_chunks());
        }
    }

    #[test]
    fn tiny_signal_scales_and_history_transitions_match_reference() {
        let planes: Vec<_> = seeded_planes(2, 385)
            .into_iter()
            .map(|plane| {
                plane
                    .into_iter()
                    .enumerate()
                    .map(|(frame, sample)| {
                        sample
                            * match frame / 55 {
                                0 => f32::MIN_POSITIVE * 0.25,
                                1 => f32::MIN_POSITIVE,
                                2 => f32::MIN_POSITIVE * 64.0,
                                3 => 1.0e-20,
                                4 => 1.0e-12,
                                5 => f32::MIN_POSITIVE * 0.5,
                                _ => 0.0,
                            }
                    })
                    .collect::<Vec<_>>()
            })
            .collect();
        for rate in [22_050, 48_000, 96_000, 192_000] {
            assert_reference(&planes, rate, &random_chunks());
        }
    }
}
