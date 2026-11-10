//! Frequency-sampled complementary lowpasses and finite quadrature FIRs with a common linear-phase delay.

use realfft::RealFftPlanner;
use realfft::num_complex::Complex32;
use thiserror::Error;

/// Crossover frequencies used before the first host request, in Hz.
pub const DEFAULT_CROSSOVERS: [f32; 3] = [100.0, 500.0, 5_000.0];
/// Shared overlap-save partition and crossover handoff length.
pub const PARTITION_SIZE: usize = 256;
/// Minimum separation between crossovers, one third of an octave.
const MIN_CROSSOVER_RATIO: f32 = 1.259_921_1;
/// Four-term Blackman-Harris window coefficients.
const BLACKMAN_HARRIS: [f64; 4] = [0.358_75, 0.488_29, 0.141_28, 0.011_68];

/// Preparation and worker-design errors, formatted only outside real-time processing.
#[derive(Debug, Error)]
pub enum MultibandError {
    /// Rates outside the supported bounded kernel geometry are rejected before allocating.
    #[error("unsupported multiband sample rate")]
    InvalidRate,
    /// The operating system could not start the kernel designer.
    #[error("could not start crossover kernel designer: {0}")]
    Worker(#[from] std::io::Error),
    /// A transform violated its fixed preparation-time buffer contract.
    #[error("crossover FFT failed: {0}")]
    Transform(#[from] realfft::FftError),
}

/// Returns the kernel half-length, doubling above each 48 kHz rate boundary.
pub fn half_length(sample_rate: f32) -> Result<usize, MultibandError> {
    if !sample_rate.is_finite() || !(1.0..=2_822_400.0).contains(&sample_rate) {
        return Err(MultibandError::InvalidRate);
    }
    let mut reference_rate = 48_000.0;
    let mut length = 4_096;
    for _ in 0..6 {
        if sample_rate <= reference_rate {
            break;
        }
        reference_rate *= 2.0;
        length *= 2;
    }
    Ok(length)
}

/// Rejects non-finite requests and orders finite controls using the shared one-third-octave rule.
pub fn order_crossovers(requested: [f32; 3]) -> Option<[f32; 3]> {
    if !requested.iter().all(|frequency| frequency.is_finite()) {
        return None;
    }
    let mut ordered = [0.0; 3];
    let mut lower = 20.0;
    for ((frequency, destination), above) in requested.into_iter().zip(&mut ordered).zip([2, 1, 0]) {
        let upper = 20_000.0 / MIN_CROSSOVER_RATIO.powi(above);
        *destination = frequency.clamp(lower, upper);
        lower = *destination * MIN_CROSSOVER_RATIO;
    }
    Some(ordered)
}

/// Tests exact request identity without discarding small host automation changes.
pub fn same_crossovers(first: [f32; 3], second: [f32; 3]) -> bool {
    first.map(f32::to_bits) == second.map(f32::to_bits)
}

/// Designs all ordinary and quadrature kernels together, allocating only in preparation or a designer.
pub fn design_kernels(
    crossovers: [f32; 3],
    sample_rate: f32,
    half_length: usize,
) -> Result<Vec<Vec<f32>>, MultibandError> {
    let mut kernels = Vec::with_capacity(7);
    for crossover in crossovers {
        kernels.push(design_kernel(sample_rate, half_length, Some(crossover), false)?);
    }
    for crossover in crossovers {
        kernels.push(design_kernel(sample_rate, half_length, Some(crossover), true)?);
    }
    kernels.push(design_kernel(sample_rate, half_length, None, true)?);
    Ok(kernels)
}

/// Samples a real-even lowpass or imaginary-odd quadrature target, then windows its centered inverse FFT.
fn design_kernel(
    sample_rate: f32,
    half_length: usize,
    crossover: Option<f32>,
    quadrature: bool,
) -> Result<Vec<f32>, MultibandError> {
    let fft_size = half_length * 4;
    let inverse = RealFftPlanner::<f32>::new().plan_fft_inverse(fft_size);
    let mut spectrum = inverse.make_input_vec();
    for (bin, value) in spectrum.iter_mut().enumerate() {
        let frequency = bin as f64 * f64::from(sample_rate) / fft_size as f64;
        let magnitude = crossover.map_or(1.0, |cutoff| lowpass_magnitude(frequency, f64::from(cutoff))) as f32;
        *value = if quadrature {
            Complex32::new(
                0.0,
                if bin == 0 || bin == fft_size / 2 {
                    0.0
                } else {
                    -magnitude
                },
            )
        } else {
            Complex32::new(magnitude, 0.0)
        };
    }
    let mut impulse = inverse.make_output_vec();
    let mut scratch = inverse.make_scratch_vec();
    inverse.process_with_scratch(&mut spectrum, &mut impulse, &mut scratch)?;
    let normalization = 1.0 / fft_size as f64;
    let length = half_length * 2 + 1;
    let mut kernel = vec![0.0; length];
    if quadrature {
        for offset in 1..=half_length {
            let positive = impulse.get(offset).copied().unwrap_or_default();
            let negative = impulse.get(fft_size - offset).copied().unwrap_or_default();
            let value = ((f64::from(positive) - f64::from(negative))
                * 0.5
                * normalization
                * blackman_harris(half_length + offset, length)) as f32;
            if let Some(tap) = kernel.get_mut(half_length + offset) {
                *tap = value;
            }
            if let Some(tap) = kernel.get_mut(half_length - offset) {
                *tap = -value;
            }
        }
    } else {
        let mut taps = Vec::with_capacity(length);
        let mut sum = 0.0;
        for tap in 0..length {
            let circular = (tap + fft_size - half_length) % fft_size;
            let value = f64::from(impulse.get(circular).copied().unwrap_or_default())
                * normalization
                * blackman_harris(tap, length);
            taps.push(value);
            sum += value;
        }
        for (destination, value) in kernel.iter_mut().zip(taps) {
            *destination = (value / sum) as f32;
        }
    }
    Ok(kernel)
}

/// Returns the one-octave raised-cosine lowpass target on a logarithmic frequency axis.
fn lowpass_magnitude(frequency: f64, crossover: f64) -> f64 {
    if frequency <= 0.0 {
        return 1.0;
    }
    let position = (frequency / crossover).log2();
    if position <= -0.5 {
        1.0
    } else if position >= 0.5 {
        0.0
    } else {
        0.5 * (1.0 - (std::f64::consts::PI * position).sin())
    }
}

/// Evaluates the shared symmetric four-term window in double precision before rounding taps.
fn blackman_harris(index: usize, length: usize) -> f64 {
    let phase = std::f64::consts::TAU * index as f64 / (length - 1) as f64;
    let mut sign = 1.0;
    let mut result = 0.0;
    for (term, coefficient) in BLACKMAN_HARRIS.into_iter().enumerate() {
        result = (sign * coefficient).mul_add((term as f64 * phase).cos(), result);
        sign = -sign;
    }
    result
}
