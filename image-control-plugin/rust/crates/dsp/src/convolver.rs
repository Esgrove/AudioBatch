//! Uniform overlap-save convolution with shared raw-input spectral history and preparation-time FFT scratch.

use std::sync::Arc;

use realfft::num_complex::Complex32;
use realfft::{ComplexToReal, RealFftPlanner, RealToComplex};

use crate::crossover::{MultibandError, PARTITION_SIZE};

/// Transform size for one current input partition and its preceding overlap.
const FFT_SIZE: usize = PARTITION_SIZE * 2;
/// Nonnegative bins in the real transform.
const BINS: usize = FFT_SIZE / 2 + 1;

/// Immutable partition spectra, transferred from the designer to the audio thread as one owned value.
pub struct KernelSpectra {
    spectra: Vec<Complex32>,
    partitions: usize,
    crossovers: [f32; 3],
}

/// One raw signal's frequency-domain delay line, reused by every ordinary and quadrature output.
pub struct PartitionedConvolver {
    forward: Arc<dyn RealToComplex<f32>>,
    inverse: Arc<dyn ComplexToReal<f32>>,
    history: Vec<Complex32>,
    window: Vec<f32>,
    transform_input: Vec<f32>,
    transform_output: Vec<f32>,
    accumulator: Vec<Complex32>,
    forward_scratch: Vec<Complex32>,
    inverse_scratch: Vec<Complex32>,
    newest: usize,
    partitions: usize,
}

impl KernelSpectra {
    /// Transforms zero-padded kernel partitions outside the real-time callback.
    pub fn new(kernels: &[Vec<f32>], crossovers: [f32; 3]) -> Result<Self, MultibandError> {
        let partitions = kernels
            .first()
            .map_or(0, |kernel| kernel.len().div_ceil(PARTITION_SIZE));
        let forward = RealFftPlanner::<f32>::new().plan_fft_forward(FFT_SIZE);
        let mut input = forward.make_input_vec();
        let mut output = forward.make_output_vec();
        let mut scratch = forward.make_scratch_vec();
        let mut spectra = Vec::with_capacity(kernels.len() * partitions * BINS);
        for kernel in kernels {
            for taps in kernel.chunks(PARTITION_SIZE) {
                input.fill(0.0);
                for (destination, &tap) in input.iter_mut().zip(taps) {
                    *destination = tap;
                }
                forward.process_with_scratch(&mut input, &mut output, &mut scratch)?;
                spectra.extend_from_slice(&output);
            }
        }
        Ok(Self {
            spectra,
            partitions,
            crossovers,
        })
    }

    /// Returns the immutable request that produced this coherent ordinary/quadrature publication.
    pub const fn crossovers(&self) -> [f32; 3] {
        self.crossovers
    }

    /// Returns the fixed raw-input history length required by these kernels.
    pub const fn partitions(&self) -> usize {
        self.partitions
    }

    /// Borrows all frequency-domain partitions of one filter without allocating.
    fn kernel(&self, index: usize) -> &[Complex32] {
        let length = self.partitions * BINS;
        self.spectra
            .get(index * length..(index + 1) * length)
            .unwrap_or_default()
    }
}

impl PartitionedConvolver {
    /// Allocates every history, transform, and scratch buffer during stopped preparation.
    pub fn new(partitions: usize) -> Self {
        let mut planner = RealFftPlanner::<f32>::new();
        let forward = planner.plan_fft_forward(FFT_SIZE);
        let inverse = planner.plan_fft_inverse(FFT_SIZE);
        Self {
            history: vec![Complex32::default(); partitions * BINS],
            window: vec![0.0; FFT_SIZE],
            transform_input: forward.make_input_vec(),
            transform_output: inverse.make_output_vec(),
            accumulator: inverse.make_input_vec(),
            forward_scratch: forward.make_scratch_vec(),
            inverse_scratch: inverse.make_scratch_vec(),
            forward,
            inverse,
            newest: 0,
            partitions,
        }
    }

    /// Clears raw history and overlap without resizing or replacing allocated storage.
    pub fn reset(&mut self) {
        self.history.fill(Complex32::default());
        self.window.fill(0.0);
        self.newest = 0;
    }

    /// Advances raw-input history once, even when all output filters are skipped at neutral controls.
    pub fn push_input(&mut self, input: &[f32; PARTITION_SIZE]) {
        self.window.copy_within(PARTITION_SIZE.., 0);
        let (_, current) = self.window.split_at_mut(PARTITION_SIZE);
        current.copy_from_slice(input);
        self.transform_input.copy_from_slice(&self.window);
        self.newest = (self.newest + 1) % self.partitions;
        if let Some(destination) = self.history.as_chunks_mut::<BINS>().0.get_mut(self.newest) {
            // Dimensions and scratch are fixed by the same plans during preparation.
            if self
                .forward
                .process_with_scratch(&mut self.transform_input, destination, &mut self.forward_scratch)
                .is_err()
            {
                destination.fill(Complex32::default());
            }
        }
    }

    /// Computes one causal FIR output without moving history, allocating, or destroying kernel storage.
    pub fn convolve(&mut self, kernels: &KernelSpectra, kernel: usize, output: &mut [f32; PARTITION_SIZE]) {
        self.accumulator.fill(Complex32::default());
        for (partition, taps) in kernels.kernel(kernel).as_chunks::<BINS>().0.iter().enumerate() {
            let slot = (self.newest + self.partitions - partition) % self.partitions;
            if let Some(input) = self.history.as_chunks::<BINS>().0.get(slot) {
                for ((sum, input), tap) in self.accumulator.iter_mut().zip(input).zip(taps) {
                    sum.re += input.im.mul_add(-tap.im, input.re * tap.re);
                    sum.im += input.im.mul_add(tap.re, input.re * tap.im);
                }
            }
        }
        // A real inverse requires exactly real DC and Nyquist, including after accumulated roundoff.
        if let Some(dc) = self.accumulator.first_mut() {
            dc.im = 0.0;
        }
        if let Some(nyquist) = self.accumulator.last_mut() {
            nyquist.im = 0.0;
        }
        if self
            .inverse
            .process_with_scratch(
                &mut self.accumulator,
                &mut self.transform_output,
                &mut self.inverse_scratch,
            )
            .is_err()
        {
            output.fill(0.0);
            return;
        }
        let (_, causal) = self.transform_output.split_at(PARTITION_SIZE);
        for (destination, &value) in output.iter_mut().zip(causal) {
            *destination = value / FFT_SIZE as f32;
        }
    }
}
