//! Framework-independent mid/side processing, atomic controls, and bounded analyzer transport.
//!
//! Complementary linear-phase bands report their real convolution latency and finite tail.
//! Mutable processing state and the SPSC producer belong exclusively to the host's processing thread.

mod analyzer;
mod convolver;
mod correlation;
mod crossover;
mod kernel_worker;
mod monitoring;
mod multiband;
mod particles;
mod worker;

use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::sync::{Arc, Mutex};

use rtrb::{Consumer, Producer, RingBuffer};
use serde::Serialize;

pub use analyzer::{
    ANALYSIS_TIER_COUNT, AnalysisError, SpatialAnalyzer, SpectralFrameInfo, SpectralPoint, StereoPositionCurve,
};
pub use crossover::order_crossovers;
use monitoring::{AnalysisSample, Monitoring};
pub use monitoring::{AnalysisStatus, AnalysisTap, ColourMode, CorrelationSnapshot, DisplayPreferences};
pub use multiband::{BandControls, MultibandError, MultibandImager};
pub use particles::SpatialParticle;
pub use particles::{
    ColorMode, DisplaySettings, FrequencyScale, ParticleBuilder, contrast_curve, frequency_to_share, share_to_frequency,
};
pub use worker::{AnalysisShutdown, AnalysisUpdate, AnalysisWorker};

/// Fixed transport capacity, independent of the host's block size.
const ANALYZER_CAPACITY: usize = 32_768;

/// Finite plain-unit controls persisted independently of any GUI or plugin framework.
#[derive(Clone, Copy, Debug, Serialize)]
pub struct Parameters {
    gain: f32,
    width: f32,
}

/// Atomic control values shared with the controller and detached editor.
///
/// The mutex protects only the consumer and is never acquired by audio processing.
/// One editor drains the queue at a time. Audio drops frames when storage is full.
pub struct SharedState {
    gain: AtomicU32,
    width: AtomicU32,
    bands: [[AtomicU32; 6]; 4],
    crossovers: [AtomicU32; 3],
    bypass: AtomicBool,
    delta: AtomicBool,
    mono: AtomicBool,
    monitoring: Monitoring,
    consumer: Mutex<Consumer<AnalysisSample>>,
}

/// Mutable audio-thread processor with allocation-free finite ramps and a bounded analyzer producer.
pub struct Imager {
    shared: Arc<SharedState>,
    producer: Producer<AnalysisSample>,
    correlation_generation: u64,
    correlation_revision: u64,
    gain: Ramp,
    width: Ramp,
    ramp_samples: u32,
    multiband: Option<MultibandImager>,
    delta_blend: Ramp,
    mono_blend: Ramp,
    channels: u8,
    offline: bool,
}

/// One audio-thread linear ramp. Retargeting starts from the current value, not the previous target.
struct Ramp {
    current: f32,
    target: f32,
    step: f32,
    remaining: u32,
}

impl Parameters {
    /// Validates gain in [0, 16] and width in [0, 2], rejecting non-finite saved or automated values.
    #[must_use]
    pub fn new(gain: f32, width: f32) -> Option<Self> {
        (gain.is_finite() && width.is_finite() && (0.0..=16.0).contains(&gain) && (0.0..=2.0).contains(&width))
            .then_some(Self { gain, width })
    }

    /// Returns linear output gain.
    #[must_use]
    pub const fn gain(self) -> f32 {
        self.gain
    }

    /// Returns the Side multiplier, where one leaves the stereo image unchanged.
    #[must_use]
    pub const fn width(self) -> f32 {
        self.width
    }
}

impl Default for Parameters {
    fn default() -> Self {
        Self { gain: 1.0, width: 1.0 }
    }
}

impl SharedState {
    /// Returns independently readable control values. This is not a multi-parameter transaction.
    #[must_use]
    pub fn parameters(&self) -> Parameters {
        Parameters {
            gain: f32::from_bits(self.gain.load(Ordering::Relaxed)),
            width: f32::from_bits(self.width.load(Ordering::Relaxed)),
        }
    }

    /// Publishes validated controls without allocation or locking.
    pub fn set_parameters(&self, parameters: Parameters) {
        self.gain.store(parameters.gain.to_bits(), Ordering::Relaxed);
        self.width.store(parameters.width.to_bits(), Ordering::Relaxed);
    }

    /// Publishes a finite normalized band field. Unknown indices and non-finite automation are ignored.
    /// Fields are Mid, Side, Width, Recover, Pan, and Solo in that stable order.
    pub fn set_band_parameter(&self, band: usize, field: usize, normalized: f32) {
        if normalized.is_finite()
            && let Some(slot) = self.bands.get(band).and_then(|fields| fields.get(field))
        {
            slot.store(normalized.clamp(0.0, 1.0).to_bits(), Ordering::Relaxed);
        }
    }

    /// Publishes a bounded crossover frequency without designing or allocating kernels on the caller.
    pub fn set_crossover(&self, index: usize, frequency: f32) {
        if frequency.is_finite()
            && let Some(slot) = self.crossovers.get(index)
        {
            slot.store(frequency.clamp(20.0, 20_000.0).to_bits(), Ordering::Relaxed);
        }
    }

    /// Selects smoothed latency-preserving bypass without replacing saved sound controls.
    pub fn set_bypass(&self, enabled: bool) {
        self.bypass.store(enabled, Ordering::Relaxed);
    }

    /// Selects latency-aligned difference monitoring.
    pub fn set_delta(&self, enabled: bool) {
        self.delta.store(enabled, Ordering::Relaxed);
    }

    /// Selects the final smoothed mono fold independently of per-band recovery.
    pub fn set_mono(&self, enabled: bool) {
        self.mono.store(enabled, Ordering::Relaxed);
    }

    /// Returns the finite normalized fields of one band for detached control publication.
    #[must_use]
    pub fn band_parameters(&self, band: usize) -> Option<[f32; 6]> {
        self.bands.get(band).map(|fields| {
            std::array::from_fn(|index| {
                fields
                    .get(index)
                    .map_or(0.0, |slot| f32::from_bits(slot.load(Ordering::Relaxed)))
            })
        })
    }

    /// Returns independently sampled crossover targets in Hz, ordered by the DSP before kernel design.
    #[must_use]
    pub fn crossovers(&self) -> [f32; 3] {
        std::array::from_fn(|index| {
            self.crossovers
                .get(index)
                .map_or(100.0, |slot| f32::from_bits(slot.load(Ordering::Relaxed)))
        })
    }

    /// Returns the most recently prepared sample rate for worker-side FFT geometry.
    #[must_use]
    pub fn sample_rate(&self) -> f32 {
        f32::from_bits(self.monitoring.rate.load(Ordering::Acquire))
    }

    /// Drains diagnostic audio only when no analysis worker owns the consumer.
    /// This bounded compatibility API never permits a diagnostic caller to steal a live worker's samples.
    pub fn drain_audio(&self, destination: &mut Vec<[f32; 2]>) {
        destination.clear();
        if self
            .monitoring
            .claimed
            .compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
            .is_err()
        {
            return;
        }
        let mut consumer = self.consumer.lock().unwrap_or_else(std::sync::PoisonError::into_inner);
        for _ in 0..ANALYZER_CAPACITY {
            let Ok(frame) = consumer.pop() else {
                break;
            };
            if frame.generation == self.analysis_generation() {
                destination.push(frame.frame);
            }
        }
        drop(consumer);
        self.monitoring.claimed.store(false, Ordering::Release);
    }

    /// Drains one bounded worker chunk while preserving source tokens. Only the claimed worker calls this.
    fn drain_analysis(&self, destination: &mut Vec<AnalysisSample>) {
        destination.clear();
        let mut consumer = self.consumer.lock().unwrap_or_else(std::sync::PoisonError::into_inner);
        for _ in 0..4096 {
            let Ok(frame) = consumer.pop() else { break };
            destination.push(frame);
        }
        drop(consumer);
    }
}

impl Imager {
    /// Allocates fixed transport storage outside the processing callback.
    #[must_use]
    pub fn new() -> Self {
        let (producer, consumer) = RingBuffer::new(ANALYZER_CAPACITY);
        Self {
            shared: Arc::new(SharedState {
                gain: AtomicU32::new(1.0_f32.to_bits()),
                width: AtomicU32::new(1.0_f32.to_bits()),
                bands: std::array::from_fn(|_| {
                    [60.0_f32 / 72.0, 60.0 / 72.0, 0.5, 0.0, 0.5, 0.0].map(|value| AtomicU32::new(value.to_bits()))
                }),
                crossovers: [100.0_f32, 500.0, 5000.0].map(|value| AtomicU32::new(value.to_bits())),
                bypass: AtomicBool::new(false),
                delta: AtomicBool::new(false),
                mono: AtomicBool::new(false),
                monitoring: Monitoring::new(),
                consumer: Mutex::new(consumer),
            }),
            producer,
            correlation_generation: 0,
            correlation_revision: 0,
            gain: Ramp::new(1.0),
            width: Ramp::new(1.0),
            ramp_samples: 960,
            multiband: None,
            delta_blend: Ramp::new(0.0),
            mono_blend: Ramp::new(0.0),
            channels: 2,
            offline: false,
        }
    }

    /// Returns the shared controls and editor transport without exposing the audio producer.
    #[must_use]
    pub fn shared(&self) -> Arc<SharedState> {
        Arc::clone(&self.shared)
    }

    /// Configures control ramps outside concurrent processing and settles the current saved targets.
    ///
    /// Accepts 1 Hz through 2,822,400 Hz at their actual rate. Unsupported rates leave preparation unchanged.
    pub fn prepare(&mut self, sample_rate: f32) -> bool {
        if !sample_rate.is_finite() || !(1.0..=2_822_400.0).contains(&sample_rate) {
            return false;
        }
        let Ok(mut multiband) = MultibandImager::new(sample_rate) else {
            return false;
        };
        if multiband.set_offline(self.offline).is_err() {
            return false;
        }
        let _ = multiband.set_channels(self.channels);
        // Preparation is non-realtime. Install saved crossover kernels before the first FIR pre-ring.
        if multiband.set_offline(true).is_err() || !multiband.set_crossovers(self.shared.crossovers()) {
            return false;
        }
        for _ in 0..256 {
            let _ = multiband.process_frame([0.0; 2]);
        }
        multiband.reset();
        if multiband.set_offline(self.offline).is_err() {
            return false;
        }
        self.multiband = Some(multiband);
        self.shared.monitoring.set_rate(sample_rate);
        self.ramp_samples = multiband::ramp_samples(sample_rate);
        self.begin_block();
        self.reset();
        true
    }

    /// Settles controls without allocating. Hosts serialize reset against processing.
    pub fn reset(&mut self) {
        self.shared.invalidate_monitoring();
        self.begin_block();
        self.gain = Ramp::new(self.gain.target);
        self.width = Ramp::new(self.width.target);
        self.delta_blend = Ramp::new(self.delta_blend.target);
        self.mono_blend = Ramp::new(self.mono_blend.target);
        if let Some(multiband) = &mut self.multiband {
            multiband.reset();
        }
    }

    /// Sets host offline rendering policy. Any synchronous design happens only on the processing owner.
    pub fn set_offline(&mut self, offline: bool) {
        if self.offline != offline {
            self.shared.invalidate_monitoring();
        }
        self.offline = offline;
        if let Some(multiband) = &mut self.multiband {
            let _ = multiband.set_offline(offline);
        }
    }

    /// Selects matching mono or stereo processing before callbacks begin.
    pub fn set_channels(&mut self, channels: usize) {
        if channels == 1 || channels == 2 {
            self.channels = channels as u8;
            if let Some(multiband) = &mut self.multiband {
                let _ = multiband.set_channels(self.channels);
            }
        }
    }

    /// Reports actual prepared audio latency, zero before preparation.
    #[must_use]
    pub fn latency_samples(&self) -> u32 {
        self.multiband
            .as_ref()
            .map_or(0, |imager| imager.latency_samples() as u32)
    }

    /// Reports finite ringing beyond the latency-compensated endpoint, zero before preparation.
    #[must_use]
    pub fn tail_samples(&self) -> u32 {
        self.multiband.as_ref().map_or(0, |imager| imager.tail_samples() as u32)
    }

    /// Samples atomic targets once at the start of a host block.
    pub fn begin_block(&mut self) {
        let parameters = self.shared.parameters();
        let bypass = self.shared.bypass.load(Ordering::Relaxed);
        self.gain
            .set_target(if bypass { 1.0 } else { parameters.gain }, self.ramp_samples);
        self.width
            .set_target(if bypass { 1.0 } else { parameters.width }, self.ramp_samples);
        self.delta_blend.set_target(
            f32::from(!bypass && self.shared.delta.load(Ordering::Relaxed)),
            self.ramp_samples,
        );
        self.mono_blend
            .set_target(f32::from(self.shared.mono.load(Ordering::Relaxed)), self.ramp_samples);
        if let Some(multiband) = &mut self.multiband {
            let _ = multiband.set_output_controls(
                if bypass { 1.0 } else { parameters.gain },
                if bypass { 1.0 } else { parameters.width },
                !bypass && self.shared.delta.load(Ordering::Relaxed),
            );
            let generation = self.shared.monitoring.correlation_generation.load(Ordering::Acquire);
            if generation != self.correlation_generation {
                multiband.set_correlation_enabled(false);
                self.correlation_generation = generation;
            }
            multiband.set_correlation_enabled(
                !self.offline && self.shared.monitoring.correlation_enabled.load(Ordering::Acquire),
            );
            let controls = std::array::from_fn(|band| {
                if bypass {
                    return BandControls::default();
                }
                let fields =
                    self.shared
                        .band_parameters(band)
                        .unwrap_or([60.0 / 72.0, 60.0 / 72.0, 0.5, 0.0, 0.5, 0.0]);
                let [mid, side, width, recover, pan, solo] = fields;
                let gain = |value: f32| {
                    if value <= 0.0 {
                        0.0
                    } else if value.to_bits() == (60.0_f32 / 72.0).to_bits() {
                        1.0
                    } else {
                        10.0_f32.powf(72.0_f32.mul_add(value, -60.0) / 20.0)
                    }
                };
                BandControls::new(
                    gain(mid),
                    gain(side),
                    width * 2.0,
                    recover,
                    pan.mul_add(2.0, -1.0),
                    solo >= 0.5,
                )
                .unwrap_or_default()
            });
            multiband.set_controls(controls);
            let _ = multiband.set_crossovers(self.shared.crossovers());
        }
    }

    /// Processes one stereo frame without allocation, locking, logging, or unbounded retries.
    /// Neutral frames bypass arithmetic to preserve every input bit, including signed zero.
    #[must_use]
    #[expect(
        clippy::manual_midpoint,
        reason = "Preserve the production plugin's mid/side operation order"
    )]
    pub fn process_frame(&mut self, frame: [f32; 2]) -> [f32; 2] {
        let source = self.shared.analysis_generation();
        let mut output = if let Some(imager) = &mut self.multiband {
            imager.process_frame(frame)
        } else {
            // The unprepared compatibility path has no partition adapter or FIR latency.
            let gain = self.gain.next();
            let width = self.width.next();
            let mut output = if width.to_bits() == 1.0_f32.to_bits() {
                if gain.to_bits() == 1.0_f32.to_bits() {
                    frame
                } else {
                    frame.map(|sample| sample * gain)
                }
            } else {
                let [left, right] = frame;
                let mid = (left + right) * 0.5;
                let side = (left - right) * (0.5 * width);
                [(mid + side) * gain, (mid - side) * gain]
            };
            let delta = self.delta_blend.next();
            if delta > 0.0 {
                for (sample, dry) in output.iter_mut().zip(frame) {
                    *sample = delta.mul_add(-dry, *sample);
                }
            }
            output
        };
        let mono = self.mono_blend.next();
        if mono > 0.0 {
            let [left, right] = output;
            let mid = (left + right) * 0.5;
            if mono.to_bits() == 1.0_f32.to_bits() && self.mono_blend.remaining == 0 {
                output = [mid; 2];
            } else {
                for sample in &mut output {
                    *sample = (mid - *sample).mul_add(mono, *sample);
                }
            }
        }
        if !self.offline && monitoring::capture_enabled(source) {
            let _ = self.producer.push(AnalysisSample {
                frame: monitoring::select_frame(source, frame, output),
                generation: source,
            });
        }
        if let Some(multiband) = &self.multiband {
            let revision = multiband.correlation_revision();
            if revision != self.correlation_revision {
                self.correlation_revision = revision;
                if self.correlation_generation == self.shared.monitoring.correlation_generation.load(Ordering::Acquire)
                {
                    for (slot, reading) in self
                        .shared
                        .monitoring
                        .readings
                        .iter()
                        .zip(multiband.correlation_readings())
                    {
                        slot.store(reading.to_bits(), Ordering::Relaxed);
                    }
                    self.shared
                        .monitoring
                        .correlation_published_generation
                        .store(self.correlation_generation, Ordering::Release);
                    self.shared
                        .monitoring
                        .correlation_revision
                        .fetch_add(1, Ordering::Release);
                }
            }
        }
        output
    }
}

impl Default for Imager {
    fn default() -> Self {
        Self::new()
    }
}

impl Ramp {
    /// Constructs a settled audio-thread ramp.
    const fn new(value: f32) -> Self {
        Self {
            current: value,
            target: value,
            step: 0.0,
            remaining: 0,
        }
    }

    /// Retargets only on an exact control change, retaining the original duration across blocks.
    fn set_target(&mut self, value: f32, samples: u32) {
        if self.target.to_bits() != value.to_bits() {
            if samples == 0 {
                *self = Self::new(value);
                return;
            }
            self.target = value;
            self.remaining = samples;
            self.step = (value - self.current) / self.remaining as f32;
        }
    }

    /// Advances once per frame and lands exactly on the requested target.
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
}

#[cfg(test)]
mod test_processing {
    use super::{Imager, Parameters};

    #[test]
    fn neutral_preserves_bits_including_signed_zero() {
        let mut imager = Imager::new();
        imager.begin_block();
        for frame in [[-0.0, 0.0], [0.123_456_79, -0.987_654_3], [f32::MIN_POSITIVE, 1.0]] {
            assert_eq!(imager.process_frame(frame).map(f32::to_bits), frame.map(f32::to_bits));
        }
    }

    #[test]
    fn narrowed_side_disappears_after_preparation() {
        let mut imager = Imager::new();
        imager
            .shared()
            .set_parameters(Parameters::new(1.0, 0.0).expect("valid controls"));
        assert!(imager.prepare(48_000.0));
        imager.begin_block();
        assert_eq!(imager.process_frame([1.0, -1.0]), [0.0, 0.0]);
    }

    #[test]
    fn malformed_controls_and_rate_are_rejected() {
        for (gain, width) in [(f32::NAN, 1.0), (1.0, f32::INFINITY), (-1.0, 1.0), (1.0, 3.0)] {
            assert!(Parameters::new(gain, width).is_none());
        }
        assert!(!Imager::new().prepare(f32::NAN));
    }

    #[test]
    fn full_analyzer_queue_drops_without_affecting_audio() {
        let mut imager = Imager::new();
        let shared = imager.shared();
        shared.set_analysis_enabled(true);
        for _ in 0..100_000 {
            assert_eq!(imager.process_frame([0.5, -0.5]), [0.5, -0.5]);
        }
        let mut frames = Vec::new();
        shared.drain_audio(&mut frames);
        assert_eq!(frames.len(), 32_768);
    }

    #[test]
    fn smoothing_duration_is_independent_of_block_boundaries() {
        let mut whole = Imager::new();
        let mut split = Imager::new();
        for imager in [&mut whole, &mut split] {
            imager
                .shared()
                .set_parameters(Parameters::new(0.5, 0.0).expect("valid"));
            imager.begin_block();
        }
        for sample in 0..2000 {
            if sample % 17 == 0 {
                split.begin_block();
            }
            assert_eq!(whole.process_frame([0.75, -0.25]), split.process_frame([0.75, -0.25]));
        }
    }
}
