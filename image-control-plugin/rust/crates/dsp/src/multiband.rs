//! Four complementary linear-phase bands with independent Mid/Side trims, Width, quadrature Recover, Pan, and Solo.
//!
//! Construction and destruction require stopped callbacks. Real-time setters, reset, and processing never allocate,
//! lock, wake a worker, or destroy heap-backed kernels. Explicit offline mode permits synchronous kernel design.

use crate::convolver::{KernelSpectra, PartitionedConvolver};
use crate::correlation::BandCorrelation;
use crate::crossover::{DEFAULT_CROSSOVERS, PARTITION_SIZE, half_length, order_crossovers, same_crossovers};
use crate::kernel_worker::{KernelWorker, design};

pub use crate::crossover::MultibandError;

/// Linear smoothing time shared by all band controls, in seconds.
const RAMP_SECONDS: f64 = 0.02;

/// Matches the reference smoother's double-precision floor, including rates with fractional ramp lengths.
pub fn ramp_samples(sample_rate: f32) -> u32 {
    (f64::from(sample_rate) * RAMP_SECONDS).floor() as u32
}

/// Validated finite plain-unit controls for one band, independent of global output gain and Width.
#[derive(Clone, Copy, Debug)]
pub struct BandControls {
    mid_gain: f32,
    side_gain: f32,
    width: f32,
    recover: f32,
    pan: f32,
    solo: bool,
}

/// Streaming audio-thread processor with fixed buffers and background crossover replacement.
pub struct MultibandImager {
    worker: KernelWorker,
    kernels: Box<KernelSpectra>,
    offline_pending: Option<Box<KernelSpectra>>,
    requested_crossovers: [f32; 3],
    mid_convolver: PartitionedConvolver,
    side_convolver: PartitionedConvolver,
    controls: [BandRamps; 4],
    output_gain: ControlRamp,
    output_width: ControlRamp,
    delta: ControlRamp,
    correlation: BandCorrelation,
    correlation_enabled: bool,
    input: [[f32; 2]; PARTITION_SIZE],
    output: [[f32; 2]; PARTITION_SIZE],
    reference: [[f32; 2]; PARTITION_SIZE],
    last_reference: [f32; 2],
    delay: Vec<[f32; 2]>,
    partition: [BandSample; PARTITION_SIZE],
    block_position: usize,
    delay_position: usize,
    half_length: usize,
    ramp_samples: u32,
    channels: u8,
    offline: bool,
}

/// All finite linear ramps for one band, including the audible factor derived from the complete Solo set.
#[derive(Clone, Copy)]
struct BandRamps {
    mid: ControlRamp,
    side: ControlRamp,
    width: ControlRamp,
    recover: ControlRamp,
    pan: ControlRamp,
    audible: ControlRamp,
}

/// Ordinary, quadrature, and dry values for one sample of the output partition.
#[derive(Clone, Copy, Default)]
struct BandSample {
    mid: [f32; 3],
    side: [f32; 3],
    quadrature: [f32; 3],
    quadrature_full: f32,
    dry: [f32; 2],
}

/// Allocation-free retargetable ramp, advancing exactly once per computed output sample.
#[derive(Clone, Copy)]
struct ControlRamp {
    current: f32,
    target: f32,
    step: f32,
    remaining: u32,
}

impl BandControls {
    /// Validates source gains in [0, 16], Width and Recover in [0, 2], and Pan in [-1, 1].
    #[must_use]
    pub fn new(mid_gain: f32, side_gain: f32, width: f32, recover: f32, pan: f32, solo: bool) -> Option<Self> {
        let values = [mid_gain, side_gain, width, recover, pan];
        (values.into_iter().all(f32::is_finite)
            && (0.0..=16.0).contains(&mid_gain)
            && (0.0..=16.0).contains(&side_gain)
            && (0.0..=2.0).contains(&width)
            && (0.0..=2.0).contains(&recover)
            && (-1.0..=1.0).contains(&pan))
        .then_some(Self {
            mid_gain,
            side_gain,
            width,
            recover,
            pan,
            solo,
        })
    }

    /// Returns the linear source Mid trim, before recovery adds its separate contribution.
    #[must_use]
    pub const fn mid_gain(self) -> f32 {
        self.mid_gain
    }

    /// Returns the linear source Side trim, shared by ordinary Side and recovery.
    #[must_use]
    pub const fn side_gain(self) -> f32 {
        self.side_gain
    }

    /// Returns independent stereo Width, applied after the source Side trim.
    #[must_use]
    pub const fn width(self) -> f32 {
        self.width
    }

    /// Returns the removed Side amplitude multiplier recovered into Mid below unity Width, up to two.
    #[must_use]
    pub const fn recover(self) -> f32 {
        self.recover
    }

    /// Returns signal-transfer Pan, negative for left and positive for right.
    #[must_use]
    pub const fn pan(self) -> f32 {
        self.pan
    }

    /// Returns whether this band requests Solo. No Solo or all Solo leaves every band audible.
    #[must_use]
    pub const fn solo(self) -> bool {
        self.solo
    }
}

impl Default for BandControls {
    fn default() -> Self {
        Self {
            mid_gain: 1.0,
            side_gain: 1.0,
            width: 1.0,
            recover: 0.0,
            pan: 0.0,
            solo: false,
        }
    }
}

impl MultibandImager {
    /// Allocates all buffers, designs initial FIRs, and starts a worker outside audio processing.
    /// Rates are bounded to 1 Hz through 2,822,400 Hz, with kernel geometry derived from the actual rate.
    ///
    /// # Errors
    /// Rejects invalid rates, operating-system thread creation failures, and design transform errors.
    pub fn new(sample_rate: f32) -> Result<Self, MultibandError> {
        let half_length = half_length(sample_rate)?;
        let kernels = design(DEFAULT_CROSSOVERS, sample_rate, half_length)?;
        let mid_convolver = PartitionedConvolver::new(kernels.partitions());
        let side_convolver = PartitionedConvolver::new(kernels.partitions());
        Ok(Self {
            worker: KernelWorker::new(DEFAULT_CROSSOVERS, sample_rate, half_length)?,
            kernels,
            offline_pending: None,
            requested_crossovers: DEFAULT_CROSSOVERS,
            mid_convolver,
            side_convolver,
            controls: [BandRamps::new(); 4],
            output_gain: ControlRamp::new(1.0),
            output_width: ControlRamp::new(1.0),
            delta: ControlRamp::new(0.0),
            correlation: BandCorrelation::new(sample_rate),
            correlation_enabled: false,
            input: [[0.0; 2]; PARTITION_SIZE],
            output: [[0.0; 2]; PARTITION_SIZE],
            reference: [[0.0; 2]; PARTITION_SIZE],
            last_reference: [0.0; 2],
            delay: vec![[0.0; 2]; half_length],
            partition: [BandSample::default(); PARTITION_SIZE],
            block_position: 0,
            delay_position: 0,
            half_length,
            ramp_samples: ramp_samples(sample_rate),
            channels: 2,
            offline: false,
        })
    }

    /// Sets finite block targets without allocation. Unchanged targets do not restart their 20 ms ramps.
    pub fn set_controls(&mut self, controls: [BandControls; 4]) {
        let any_solo = controls.iter().any(|control| control.solo);
        for (ramps, control) in self.controls.iter_mut().zip(controls) {
            ramps.mid.set_target(control.mid_gain, self.ramp_samples);
            ramps.side.set_target(control.side_gain, self.ramp_samples);
            ramps.width.set_target(control.width, self.ramp_samples);
            ramps.recover.set_target(control.recover, self.ramp_samples);
            ramps.pan.set_target(control.pan, self.ramp_samples);
            ramps
                .audible
                .set_target(f32::from(!any_solo || control.solo), self.ramp_samples);
        }
    }

    /// Sets validated output trim, compatibility Width, and Delta targets on the processing owner.
    /// These ramps advance with band controls while computing the next partition, never on returned old audio.
    /// Default controls preserve the existing band-only API and its exact-neutral delayed output.
    /// Returns false for non-finite or out-of-range gain/Width, retaining every previous target.
    pub fn set_output_controls(&mut self, gain: f32, width: f32, delta: bool) -> bool {
        if !gain.is_finite() || !width.is_finite() || !(0.0..=16.0).contains(&gain) || !(0.0..=2.0).contains(&width) {
            return false;
        }
        self.output_gain.set_target(gain, self.ramp_samples);
        self.output_width.set_target(width, self.ramp_samples);
        self.delta.set_target(f32::from(delta), self.ramp_samples);
        true
    }

    /// Enables observation only while requested by the editor or diagnostics, without changing audio history.
    /// Every enablement change invalidates meter history. Repeating the same request preserves accumulation.
    /// Only the processing owner calls this allocation-free setter, serialized against processing and reset.
    pub fn set_correlation_enabled(&mut self, enabled: bool) {
        if self.correlation_enabled != enabled {
            self.correlation_enabled = enabled;
            self.correlation.reset();
        }
    }

    /// Returns the latest partition's processed-band correlations, with NaN for unavailable channels.
    /// Reads processing-thread-owned state, independent of the analyzer tap and global Delta or Mono controls.
    #[must_use]
    pub const fn correlation_readings(&self) -> [f32; 4] {
        self.correlation.readings()
    }

    /// Returns a wrapping revision advanced on each measured partition and lifecycle invalidation.
    /// The processing owner can use this to publish shared atomics once per partition rather than once per frame.
    #[must_use]
    pub const fn correlation_revision(&self) -> u64 {
        self.correlation.revision()
    }

    /// Orders and publishes a finite request with no real-time allocation or callback-side waiting.
    /// Explicit offline mode designs synchronously here, before the next partition boundary.
    /// Returns false for invalid requests or an offline design error, retaining the accepted request.
    pub fn set_crossovers(&mut self, crossovers: [f32; 3]) -> bool {
        let Some(ordered) = order_crossovers(crossovers) else {
            return false;
        };
        if same_crossovers(ordered, self.requested_crossovers) {
            return true;
        }
        if self.offline {
            let Ok(kernels) = self.worker.design_offline(ordered) else {
                return false;
            };
            self.offline_pending = Some(kernels);
        }
        self.requested_crossovers = ordered;
        self.worker.request(ordered);
        true
    }

    /// Switches explicit rendering mode outside real-time callback admission.
    /// Entering offline mode serializes with in-flight design and makes the latest request deterministic.
    ///
    /// # Errors
    /// Returns an offline transform failure before processing uses a replacement.
    pub fn set_offline(&mut self, offline: bool) -> Result<(), MultibandError> {
        if self.offline == offline {
            return Ok(());
        }
        self.worker.set_offline(offline);
        self.offline = offline;
        // Mode changes run outside callback admission, so unused offline allocations can be destroyed here.
        self.offline_pending = None;
        if offline && !same_crossovers(self.kernels.crossovers(), self.requested_crossovers) {
            self.offline_pending = Some(self.worker.design_offline(self.requested_crossovers)?);
        }
        Ok(())
    }

    /// Selects matching mono or stereo processing. Hosts change layouts only with callbacks stopped.
    /// A valid change clears history and settles targets without allocating.
    pub fn set_channels(&mut self, channels: u8) -> bool {
        if !(1..=2).contains(&channels) {
            return false;
        }
        if self.channels != channels {
            self.channels = channels;
            self.reset();
        }
        true
    }

    /// Clears signal history and settles all current control targets without changing crossover requests.
    /// Hosts serialize reset against processing, and no worker accesses these audio-owned buffers.
    pub fn reset(&mut self) {
        self.input.fill([0.0; 2]);
        self.output.fill([0.0; 2]);
        self.reference.fill([0.0; 2]);
        self.last_reference = [0.0; 2];
        self.delay.fill([0.0; 2]);
        self.partition.fill(BandSample::default());
        self.mid_convolver.reset();
        self.side_convolver.reset();
        self.correlation.reset();
        for ramps in &mut self.controls {
            ramps.settle();
        }
        for ramp in [&mut self.output_gain, &mut self.output_width, &mut self.delta] {
            *ramp = ControlRamp::new(ramp.target);
        }
        self.block_position = 0;
        self.delay_position = 0;
    }

    /// Processes one frame through an arbitrary-block adapter with exactly H + 256 samples of latency.
    /// In mono mode only the first input is read, and both returned values contain the processed mono sample.
    #[must_use]
    pub fn process_frame(&mut self, frame: [f32; 2]) -> [f32; 2] {
        let output = self.output.get(self.block_position).copied().unwrap_or_default();
        // Retain this frame's reference before a completed input partition overwrites the output buffers.
        self.last_reference = self.reference.get(self.block_position).copied().unwrap_or_default();
        if let Some(destination) = self.input.get_mut(self.block_position) {
            *destination = frame;
        }
        self.block_position += 1;
        if self.block_position == PARTITION_SIZE {
            self.process_partition();
            self.block_position = 0;
        }
        output
    }

    /// Returns the ordinary dry reference aligned with the most recent `process_frame` or `process_mono` output.
    /// Only the same smoothed Solo factors affect it, not source trims, Width, Recover, Pan, or global controls.
    /// Reads audio-thread-owned state without processing, advancing ramps, allocation, or locks. Reset returns silence.
    #[must_use]
    pub const fn dry_reference(&self) -> [f32; 2] {
        self.last_reference
    }

    /// Processes a mono sample after `set_channels(1)`, ignoring stereo-only Width, Recover, and Pan.
    #[must_use]
    pub fn process_mono(&mut self, sample: f32) -> f32 {
        let [output, _] = self.process_frame([sample, sample]);
        output
    }

    /// Returns the FIR center delay plus one adapter partition, independent of controls and crossover requests.
    #[must_use]
    pub const fn latency_samples(&self) -> usize {
        self.half_length + PARTITION_SIZE
    }

    /// Returns the conservative FIR ringing beyond the latency-compensated endpoint.
    #[must_use]
    pub const fn tail_samples(&self) -> usize {
        self.half_length
    }

    /// Returns the kernels most recently activated at a processing partition boundary.
    #[must_use]
    pub fn active_crossovers(&self) -> [f32; 3] {
        self.kernels.crossovers()
    }

    /// Reports a background design failure without logging or formatting on the processing thread.
    #[must_use]
    pub fn kernel_worker_failed(&self) -> bool {
        self.worker.failed()
    }

    /// Advances shared histories and computes the next partition with one coherent crossover handoff.
    #[expect(
        clippy::manual_midpoint,
        reason = "The Mid/Side arithmetic order is part of the signal contract"
    )]
    fn process_partition(&mut self) {
        let stereo = self.channels == 2;
        let mut mid = [0.0; PARTITION_SIZE];
        let mut side = [0.0; PARTITION_SIZE];
        for (((&frame, data), mid), side) in self.input.iter().zip(&mut self.partition).zip(&mut mid).zip(&mut side) {
            if let Some(delayed) = self.delay.get_mut(self.delay_position) {
                data.dry = *delayed;
                *delayed = frame;
            }
            self.delay_position = (self.delay_position + 1) % self.half_length;
            let [left, right] = frame;
            *mid = if stereo { (left + right) * 0.5 } else { left };
            *side = if stereo { (left - right) * 0.5 } else { 0.0 };
        }
        self.mid_convolver.push_input(&mid);
        // Keep raw Side history warm even when no recovery output is required.
        self.side_convolver.push_input(&side);
        let replacement = self
            .offline_pending
            .take()
            .or_else(|| if self.offline { None } else { self.worker.take_ready() });
        let neutral = self.controls.iter().all(|ramps| ramps.neutral(stereo))
            && self.output_gain.settled_at(1.0)
            && (!stereo || self.output_width.settled_at(1.0))
            && (self.delta.settled_at(0.0) || self.delta.settled_at(1.0));
        let recovering = stereo && self.controls.iter().any(BandRamps::recovering);
        if !neutral || self.correlation_enabled {
            self.convolve_partition(replacement.as_deref(), stereo, recovering);
        }
        if self.correlation_enabled {
            self.observe_correlation(stereo, recovering);
        }
        if let Some(replacement) = replacement {
            let retired = std::mem::replace(&mut self.kernels, replacement);
            if self.offline {
                drop(retired);
            } else {
                self.worker.retire(retired);
            }
        }
        if neutral {
            for ((destination, reference), data) in self.output.iter_mut().zip(&mut self.reference).zip(&self.partition)
            {
                let [left, right] = data.dry;
                *reference = [left, if stereo { right } else { left }];
                *destination = if self.delta.settled_at(1.0) {
                    [0.0; 2]
                } else {
                    *reference
                };
            }
            for ramps in &mut self.controls {
                ramps.advance_partition();
            }
            for _ in 0..PARTITION_SIZE {
                self.output_width.next();
            }
        } else {
            self.mix_partition(stereo, recovering);
        }
    }

    /// Evaluates all required filters using the same raw histories and replacement fade progression.
    fn convolve_partition(&mut self, replacement: Option<&KernelSpectra>, stereo: bool, recovering: bool) {
        let mut output = [0.0; PARTITION_SIZE];
        let mut fade = [0.0; PARTITION_SIZE];
        for kernel in 0..3 {
            convolve_signal(
                &mut self.mid_convolver,
                &self.kernels,
                replacement,
                kernel,
                &mut output,
                &mut fade,
            );
            for (data, &value) in self.partition.iter_mut().zip(&output) {
                if let Some(destination) = data.mid.get_mut(kernel) {
                    *destination = value;
                }
            }
            if stereo {
                convolve_signal(
                    &mut self.side_convolver,
                    &self.kernels,
                    replacement,
                    kernel,
                    &mut output,
                    &mut fade,
                );
                for (data, &value) in self.partition.iter_mut().zip(&output) {
                    if let Some(destination) = data.side.get_mut(kernel) {
                        *destination = value;
                    }
                }
            }
            if recovering {
                convolve_signal(
                    &mut self.side_convolver,
                    &self.kernels,
                    replacement,
                    kernel + 3,
                    &mut output,
                    &mut fade,
                );
                for (data, &value) in self.partition.iter_mut().zip(&output) {
                    if let Some(destination) = data.quadrature.get_mut(kernel) {
                        *destination = value;
                    }
                }
            }
        }
        if recovering {
            convolve_signal(
                &mut self.side_convolver,
                &self.kernels,
                replacement,
                6,
                &mut output,
                &mut fade,
            );
            for (data, &value) in self.partition.iter_mut().zip(&output) {
                data.quadrature_full = value;
            }
        }
    }

    /// Observes existing filter outputs with copied control ramps, leaving the audio path's arithmetic untouched.
    /// Neutral monitoring still computes band outputs, but exact delayed PCM and ramp advancement remain independent.
    #[expect(
        clippy::manual_midpoint,
        reason = "The meter observes the audio path's Mid/Side arithmetic order"
    )]
    fn observe_correlation(&mut self, stereo: bool, recovering: bool) {
        let mut controls = self.controls;
        for data in &self.partition {
            let [dry_left, dry_right] = data.dry;
            let dry_mid = if stereo { (dry_left + dry_right) * 0.5 } else { dry_left };
            let dry_side = if stereo { (dry_left - dry_right) * 0.5 } else { 0.0 };
            for (band, ramps) in controls.iter_mut().enumerate() {
                let mut mid_gain = ramps.mid.next();
                let source_side = ramps.side.next();
                let width = ramps.width.next();
                let recover = ramps.recover.next();
                let pan = ramps.pan.next();
                let audible = ramps.audible.next();
                let ordinary_mid = band_sample(data.mid, dry_mid, band);
                let ordinary_side = if stereo {
                    band_sample(data.side, dry_side, band)
                } else {
                    0.0
                };
                let mut side_gain = source_side * width;
                let mut recovery_gain = source_side * recover * (1.0 - width).max(0.0);
                mid_gain *= audible;
                side_gain *= audible;
                recovery_gain *= audible;
                let mid = mid_gain * ordinary_mid;
                let side = if stereo { side_gain * ordinary_side } else { 0.0 };
                let recovered = if recovering {
                    recovery_gain * band_sample(data.quadrature, data.quadrature_full, band)
                } else {
                    0.0
                };
                let [left, right] = if stereo {
                    let left = (mid + recovered) + side;
                    let right = (mid + recovered) - side;
                    let to_right = pan.max(0.0);
                    let to_left = (-pan).max(0.0);
                    [
                        right.mul_add(to_left, left * (1.0 - to_right)),
                        left.mul_add(to_right, right * (1.0 - to_left)),
                    ]
                } else {
                    [mid; 2]
                };
                self.correlation.push(band, left, right);
            }
        }
        self.correlation.publish();
    }

    /// Mixes processed bands and their ordinary Solo-only dry reference from the same convolved partition.
    /// Both sums share each audible ramp value, while fully audible partitions retain the exact delayed original.
    /// Every control advances exactly once per sample, without allocating or adding convolution history.
    #[expect(
        clippy::manual_midpoint,
        clippy::suboptimal_flops,
        reason = "Preserve the reference Mid/Side and output-gain/Delta evaluation order"
    )]
    fn mix_partition(&mut self, stereo: bool, recovering: bool) {
        let centered = self.controls.iter().all(|ramps| ramps.pan.settled_at(0.0));
        let neutral_output = self.output_gain.settled_at(1.0)
            && (!stereo || self.output_width.settled_at(1.0))
            && (self.delta.settled_at(0.0) || self.delta.settled_at(1.0));
        for ((destination, reference), data) in self.output.iter_mut().zip(&mut self.reference).zip(&self.partition) {
            let [dry_left, dry_right] = data.dry;
            let dry_mid = if stereo { (dry_left + dry_right) * 0.5 } else { dry_left };
            let dry_side = if stereo { (dry_left - dry_right) * 0.5 } else { 0.0 };
            let mut mid_sum = 0.0;
            let mut side_sum = 0.0;
            let mut recovered_sum = 0.0;
            let mut left_sum = 0.0;
            let mut right_sum = 0.0;
            let mut reference_mid = 0.0;
            let mut reference_side = 0.0;
            let mut unsoloed = true;
            for (band, ramps) in self.controls.iter_mut().enumerate() {
                let mut mid_gain = ramps.mid.next();
                let source_side = ramps.side.next();
                let width = ramps.width.next();
                let recover = ramps.recover.next();
                let pan = ramps.pan.next();
                let audible = ramps.audible.next();
                unsoloed &= ramps.audible.settled_at(1.0);
                let ordinary_mid = band_sample(data.mid, dry_mid, band);
                let ordinary_side = if stereo {
                    band_sample(data.side, dry_side, band)
                } else {
                    0.0
                };
                let selected_mid = audible * ordinary_mid;
                let selected_side = audible * ordinary_side;
                reference_mid += selected_mid;
                reference_side += selected_side;
                let mut side_gain = source_side * width;
                let mut recovery_gain = source_side * recover * (1.0 - width).max(0.0);
                mid_gain *= audible;
                side_gain *= audible;
                recovery_gain *= audible;
                let mid = mid_gain * ordinary_mid;
                let side = if stereo { side_gain * ordinary_side } else { 0.0 };
                let recovered = if recovering {
                    recovery_gain * band_sample(data.quadrature, data.quadrature_full, band)
                } else {
                    0.0
                };
                if centered || !stereo {
                    mid_sum += mid;
                    side_sum += side;
                    recovered_sum += recovered;
                } else {
                    let left = (mid + recovered) + side;
                    let right = (mid + recovered) - side;
                    let to_right = pan.max(0.0);
                    let to_left = (-pan).max(0.0);
                    left_sum += right.mul_add(to_left, left * (1.0 - to_right));
                    right_sum += left.mul_add(to_right, right * (1.0 - to_left));
                }
            }
            let gain = self.output_gain.next();
            let width = self.output_width.next();
            let delta = self.delta.next();
            *destination = if neutral_output && unsoloed && self.controls.iter().all(|ramps| ramps.neutral(stereo)) {
                [dry_left, if stereo { dry_right } else { dry_left }]
            } else if !stereo {
                [mid_sum; 2]
            } else if centered {
                let mid = if recovering { mid_sum + recovered_sum } else { mid_sum };
                [mid + side_sum, mid - side_sum]
            } else {
                [left_sum, right_sum]
            };
            *reference = if unsoloed {
                [dry_left, if stereo { dry_right } else { dry_left }]
            } else if stereo {
                [reference_mid + reference_side, reference_mid - reference_side]
            } else {
                [reference_mid; 2]
            };
            // Unity compatibility Width must not introduce another Mid/Side cancellation after band reconstruction.
            if stereo && width.to_bits() != 1.0_f32.to_bits() {
                let [left, right] = *destination;
                let mid = (left + right) * 0.5;
                let side = (left - right) * (0.5 * width);
                *destination = [mid + side, mid - side];
            }
            if gain.to_bits() != 1.0_f32.to_bits() || delta.to_bits() != 0.0_f32.to_bits() {
                for (sample, &dry) in destination.iter_mut().zip(reference.iter()) {
                    *sample = *sample * gain - delta * dry;
                }
            }
        }
    }
}

impl BandRamps {
    /// Starts with neutral controls and every band audible.
    const fn new() -> Self {
        Self {
            mid: ControlRamp::new(1.0),
            side: ControlRamp::new(1.0),
            width: ControlRamp::new(1.0),
            recover: ControlRamp::new(0.0),
            pan: ControlRamp::new(0.0),
            audible: ControlRamp::new(1.0),
        }
    }

    /// Settles even currently ineffective stereo controls, so cold starts apply saved targets to pre-ringing.
    fn settle(&mut self) {
        for ramp in [
            &mut self.mid,
            &mut self.side,
            &mut self.width,
            &mut self.recover,
            &mut self.pan,
            &mut self.audible,
        ] {
            *ramp = ControlRamp::new(ramp.target);
        }
    }

    /// Tests the exact-neutral shortcut, ignoring controls with no contribution in mono.
    const fn neutral(&self, stereo: bool) -> bool {
        self.mid.settled_at(1.0)
            && self.audible.settled_at(1.0)
            && (!stereo || (self.side.settled_at(1.0) && self.width.settled_at(1.0) && self.pan.settled_at(0.0)))
    }

    /// Includes current and target ramps, keeping activation and deactivation histories continuous.
    fn recovering(&self) -> bool {
        self.width.current.min(self.width.target) < 1.0 && self.recover.current.max(self.recover.target) > 0.0
    }

    /// Advances ineffective control ramps even when exact-neutral output needs no filter outputs.
    fn advance_partition(&mut self) {
        for _ in 0..PARTITION_SIZE {
            self.mid.next();
            self.side.next();
            self.width.next();
            self.recover.next();
            self.pan.next();
            self.audible.next();
        }
    }
}

impl ControlRamp {
    /// Constructs a settled finite control value.
    const fn new(value: f32) -> Self {
        Self {
            current: value,
            target: value,
            step: 0.0,
            remaining: 0,
        }
    }

    /// Retargets from the current value only when a finite block target actually changes.
    fn set_target(&mut self, value: f32, samples: u32) {
        if value.to_bits() != self.target.to_bits() {
            if samples == 0 {
                *self = Self::new(value);
                return;
            }
            self.target = value;
            self.remaining = samples;
            self.step = (value - self.current) / self.remaining as f32;
        }
    }

    /// Advances once and lands exactly at the target to permit exact-neutral copy after settling.
    fn next(&mut self) -> f32 {
        if self.remaining != 0 {
            self.remaining -= 1;
            self.current = if self.remaining == 0 {
                self.target
            } else {
                self.current + self.step
            };
        }
        self.current
    }

    /// Tests exact settled identity, which is safe because output copies original samples rather than cancelling bands.
    const fn settled_at(&self, value: f32) -> bool {
        self.remaining == 0 && self.current.to_bits() == value.to_bits()
    }
}

/// Returns one amplitude-complementary band from the common-delay lowpass outputs.
fn band_sample([low, low_mid, high_mid]: [f32; 3], delayed: f32, band: usize) -> f32 {
    match band {
        0 => low,
        1 => low_mid - low,
        2 => high_mid - low_mid,
        _ => delayed - high_mid,
    }
}

/// Convolves both coherent kernel generations and crossfades over exactly one 256-sample partition.
fn convolve_signal(
    convolver: &mut PartitionedConvolver,
    kernels: &KernelSpectra,
    replacement: Option<&KernelSpectra>,
    kernel: usize,
    output: &mut [f32; PARTITION_SIZE],
    scratch: &mut [f32; PARTITION_SIZE],
) {
    convolver.convolve(kernels, kernel, output);
    if let Some(replacement) = replacement {
        convolver.convolve(replacement, kernel, scratch);
        for (index, (destination, &replacement)) in output.iter_mut().zip(scratch.iter()).enumerate() {
            let fade = (index + 1) as f32 / PARTITION_SIZE as f32;
            *destination = (replacement - *destination).mul_add(fade, *destination);
        }
    }
}
