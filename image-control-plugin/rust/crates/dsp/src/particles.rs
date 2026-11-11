//! Framework-independent, bounded spatial-particle construction with shared audio-time adaptive level scaling.

use crate::analyzer::{ANALYSIS_TIER_COUNT, SpectralFrameInfo, SpectralPoint};

/// Maximum retained display work. Overflow retains the newest tail instead of stale observations.
pub const MAXIMUM_PARTICLES: usize = 65_536;
/// Maximum copied observations per tier and deadline, including externally supplied frames.
pub const MAXIMUM_FRAME_POINTS: usize = 65_536;
const MIN_REFERENCE_DB: f32 = -90.0;
const DISPLAY_RANGE_DB: f32 = 36.0;
const RELEASE_DB_PER_SECOND: f64 = 12.0;
const DEFAULT_PERSISTENCE_SECONDS: f64 = 0.3;
const FLASH_SECONDS: f64 = 0.04;
const PALETTE_STEPS: usize = 1024;
const MIN_PARTICLE_WEIGHT: f32 = 1.0e-4;
const FREQUENCY_STOPS: [(f64, [f32; 3]); 11] = [
    (20.0, [1.0, 0.06, 0.04]),
    (50.0, [1.0, 0.1, 0.04]),
    (75.0, [1.0, 0.6, 0.05]),
    (150.0, [1.0, 0.92, 0.1]),
    (400.0, [0.92, 1.0, 0.1]),
    (1000.0, [0.2, 1.0, 0.15]),
    (2500.0, [0.1, 1.0, 0.7]),
    (4000.0, [0.1, 0.9, 1.0]),
    (8000.0, [0.2, 0.45, 1.0]),
    (13_000.0, [0.6, 0.3, 1.0]),
    (20_000.0, [0.95, 0.3, 1.0]),
];

/// Frequency-palette or phase-coherence coloring, both expressed in linear light.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum ColorMode {
    /// Reference bass-red to treble-violet palette.
    #[default]
    Frequency,
    /// In-phase blue through opposite-polarity red.
    Phase,
}

/// Horizontal projection shared with frequency-axis overlays.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum FrequencyScale {
    /// Equal horizontal width for each octave.
    #[default]
    Logarithmic,
    /// HTK mel projection with reduced low-frequency width.
    Mel,
}

impl FrequencyScale {
    /// Projects a finite positive frequency onto the 20 Hz through 20 kHz axis without clamping.
    /// This pure helper is shared by worker particles, editor overlays, and input geometry.
    #[must_use]
    pub fn normalized(self, frequency: f64) -> f64 {
        frequency_to_share(frequency, self)
    }

    /// Inverts the axis share without clamping, so overlays and hit testing share particle geometry.
    #[must_use]
    pub fn frequency(self, normalized: f64) -> f64 {
        share_to_frequency(normalized, self)
    }
}

/// Finite, range-validated settings applied only to worker-side display construction.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct DisplaySettings {
    color_mode: ColorMode,
    frequency_scale: FrequencyScale,
    persistence_seconds: f64,
    contrast: f32,
}

/// Ready-to-project particle values, independent of any graphics API or platform UI.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SpatialParticle {
    position: [f32; 2],
    color: [f32; 3],
    emphasis: f32,
    spread: f32,
    size: f32,
}

/// Reusable observations retained until every tier due at the shared deadline is known.
struct PendingFrame {
    points: Vec<(SpectralPoint, f32)>,
    frame_seconds: f64,
}

/// Worker-owned particle builder. No graphics context, locks, or host/plugin interfaces are involved.
pub struct ParticleBuilder {
    settings: DisplaySettings,
    collected: Vec<SpatialParticle>,
    projected: Vec<SpatialParticle>,
    frequency_colors: Vec<[f32; 3]>,
    pending: [PendingFrame; ANALYSIS_TIER_COUNT],
    pending_order: Vec<usize>,
    tier_peaks: [f32; ANALYSIS_TIER_COUNT],
    reference_decibels: f32,
    last_end_seconds: f64,
    pending_end_seconds: f64,
}

impl Default for DisplaySettings {
    /// Uses the reference frequency palette, logarithmic axis, 0.3-second persistence, and 60% contrast.
    fn default() -> Self {
        Self {
            color_mode: ColorMode::Frequency,
            frequency_scale: FrequencyScale::Logarithmic,
            persistence_seconds: DEFAULT_PERSISTENCE_SECONDS,
            contrast: 0.6,
        }
    }
}

impl DisplaySettings {
    /// Validates persistence from 0.05 to 3 seconds and normalized contrast from zero to one.
    #[must_use]
    pub fn new(
        color_mode: ColorMode,
        frequency_scale: FrequencyScale,
        persistence_seconds: f64,
        contrast: f32,
    ) -> Option<Self> {
        (persistence_seconds.is_finite()
            && contrast.is_finite()
            && (0.05..=3.0).contains(&persistence_seconds)
            && (0.0..=1.0).contains(&contrast))
        .then_some(Self {
            color_mode,
            frequency_scale,
            persistence_seconds,
            contrast,
        })
    }

    /// Returns the selected linear-light coloring mode.
    #[must_use]
    pub const fn color_mode(self) -> ColorMode {
        self.color_mode
    }

    /// Returns the horizontal frequency projection.
    #[must_use]
    pub const fn frequency_scale(self) -> FrequencyScale {
        self.frequency_scale
    }

    /// Returns the exponential persistence time constant in seconds.
    #[must_use]
    pub const fn persistence_seconds(self) -> f64 {
        self.persistence_seconds
    }

    /// Returns normalized contrast, where zero is linear in displayed dB.
    #[must_use]
    pub const fn contrast(self) -> f32 {
        self.contrast
    }
}

impl SpatialParticle {
    /// Returns normalized frequency-axis and top-to-bottom stereo coordinates.
    #[must_use]
    pub const fn position(self) -> [f32; 2] {
        self.position
    }

    /// Returns linear-light, flash-weighted additive color energy.
    #[must_use]
    pub const fn color(self) -> [f32; 3] {
        self.color
    }

    /// Returns contrast emphasis from zero to one for halo shaping.
    #[must_use]
    pub const fn emphasis(self) -> f32 {
        self.emphasis
    }

    /// Returns horizontal Gaussian spread as a fraction of the plot width.
    #[must_use]
    pub const fn spread(self) -> f32 {
        self.spread
    }

    /// Returns relative core size from one half to one.
    #[must_use]
    pub const fn size(self) -> f32 {
        self.size
    }
}

impl Default for ParticleBuilder {
    /// Creates empty reusable storage and precomputes the reference linear-light palette.
    fn default() -> Self {
        Self::new()
    }
}

impl ParticleBuilder {
    /// Allocates bounded reusable frame and particle storage on the non-realtime worker setup path.
    #[must_use]
    pub fn new() -> Self {
        Self {
            settings: DisplaySettings::default(),
            collected: Vec::with_capacity(MAXIMUM_PARTICLES),
            projected: Vec::with_capacity(MAXIMUM_FRAME_POINTS),
            frequency_colors: (0..PALETTE_STEPS)
                .map(|step| frequency_color(20.0 * 1000.0_f64.powf(step as f64 / (PALETTE_STEPS - 1) as f64)))
                .collect(),
            pending: std::array::from_fn(|_| PendingFrame {
                points: Vec::with_capacity(MAXIMUM_FRAME_POINTS),
                frame_seconds: 0.0,
            }),
            pending_order: Vec::with_capacity(ANALYSIS_TIER_COUNT),
            tier_peaks: [MIN_REFERENCE_DB; ANALYSIS_TIER_COUNT],
            reference_decibels: MIN_REFERENCE_DB,
            last_end_seconds: 0.0,
            pending_end_seconds: 0.0,
        }
    }

    /// Clears particles, pending groups, and adaptive history at a stream discontinuity, preserving settings.
    pub fn reset(&mut self) {
        self.collected.clear();
        self.projected.clear();
        self.pending_order.clear();
        for frame in &mut self.pending {
            frame.points.clear();
        }
        self.tier_peaks.fill(MIN_REFERENCE_DB);
        self.reference_decibels = MIN_REFERENCE_DB;
        self.last_end_seconds = 0.0;
        self.pending_end_seconds = 0.0;
    }

    /// Applies already-validated settings without clearing the audio clock or adaptive peaks.
    pub const fn set_settings(&mut self, settings: DisplaySettings) {
        self.settings = settings;
    }

    /// Returns the currently applied display settings.
    #[must_use]
    pub const fn settings(&self) -> DisplaySettings {
        self.settings
    }

    /// Returns the tilted dB reference, useful for independent adaptive-level contract tests.
    #[must_use]
    pub const fn reference_level(&self) -> f32 {
        self.reference_decibels
    }

    /// Retains one tier's frame until the deadline group closes, then colors every due tier with one reference.
    /// Invalid, duplicate, or non-monotonic frame metadata is ignored without changing adaptive history.
    /// Each tier retains its latest peak between updates. Audio time, not tier count, drives the release.
    pub fn add_frame(&mut self, points: &[SpectralPoint], frame: SpectralFrameInfo) {
        if frame.tier_index >= ANALYSIS_TIER_COUNT
            || !frame.frame_seconds.is_finite()
            || frame.frame_seconds <= 0.0
            || !frame.end_seconds.is_finite()
            || frame.end_seconds <= self.last_end_seconds
        {
            return;
        }
        if !self.pending_order.is_empty() && frame.end_seconds.to_bits() != self.pending_end_seconds.to_bits() {
            if frame.end_seconds < self.pending_end_seconds {
                return;
            }
            self.finish_deadline();
        }
        if self.pending_order.contains(&frame.tier_index) {
            return;
        }
        let Some(pending) = self.pending.get_mut(frame.tier_index) else {
            return;
        };
        pending.points.clear();
        pending.frame_seconds = frame.frame_seconds;
        let mut peak = MIN_REFERENCE_DB;
        let retained_start = points.len().saturating_sub(MAXIMUM_FRAME_POINTS);
        for (index, &point) in points.iter().enumerate() {
            let tilted = tilted_decibels(point);
            peak = peak.max(tilted);
            if index >= retained_start {
                pending.points.push((point, tilted));
            }
        }
        if let Some(tier_peak) = self.tier_peaks.get_mut(frame.tier_index) {
            *tier_peak = peak;
        }
        self.pending_order.push(frame.tier_index);
        self.pending_end_seconds = frame.end_seconds;
        if frame.last_at_time {
            self.finish_deadline();
        }
    }

    /// Returns completed particles. Pending incomplete groups are not implicitly finalized by display refresh.
    #[must_use]
    pub fn particles(&self) -> &[SpatialParticle] {
        &self.collected
    }

    /// Swaps completed particles into caller-owned reusable storage without copying.
    pub fn drain(&mut self, destination: &mut Vec<SpatialParticle>) {
        destination.clear();
        std::mem::swap(destination, &mut self.collected);
    }

    /// Drops completed particles while preserving allocation and adaptive history.
    pub fn clear(&mut self) {
        self.collected.clear();
    }

    /// Advances the release once for a shared endpoint and projects the pending tiers in arrival order.
    fn finish_deadline(&mut self) {
        let elapsed = self.pending_end_seconds - self.last_end_seconds;
        let released = RELEASE_DB_PER_SECOND.mul_add(-elapsed, f64::from(self.reference_decibels)) as f32;
        let loudest = self.tier_peaks.iter().copied().fold(MIN_REFERENCE_DB, f32::max);
        self.reference_decibels = released.max(loudest);
        self.last_end_seconds = self.pending_end_seconds;
        for &tier in &self.pending_order {
            let Some(frame) = self.pending.get(tier) else {
                continue;
            };
            self.projected.clear();
            let flash = ((1.0 - (-frame.frame_seconds / FLASH_SECONDS).exp())
                * (DEFAULT_PERSISTENCE_SECONDS / self.settings.persistence_seconds).sqrt())
                as f32;
            for &(point, decibels) in &frame.points {
                let share =
                    ((decibels - (self.reference_decibels - DISPLAY_RANGE_DB)) / DISPLAY_RANGE_DB).clamp(0.0, 1.0);
                let high_ramp = (point.frequency() / 1000.0).log2() * 0.25;
                let contrast = 0.25_f32.mul_add(high_ramp.clamp(0.0, 1.0), self.settings.contrast);
                let emphasis = contrast_curve(share, contrast);
                let weight = emphasis * flash;
                if weight < MIN_PARTICLE_WEIGHT {
                    continue;
                }
                let frequency = f64::from(point.reassigned_frequency());
                let half_spacing = f64::from(point.spacing()) * 0.5;
                let spacing = frequency_to_share(frequency + half_spacing, self.settings.frequency_scale)
                    - frequency_to_share((frequency - half_spacing).max(20.0), self.settings.frequency_scale);
                let color = match self.settings.color_mode {
                    ColorMode::Frequency => {
                        let share = frequency_to_share(f64::from(point.frequency()), FrequencyScale::Logarithmic)
                            .clamp(0.0, 1.0);
                        self.frequency_colors
                            .get((share * (PALETTE_STEPS - 1) as f64) as usize)
                            .copied()
                            .unwrap_or_default()
                    }
                    ColorMode::Phase => {
                        let anti_phase_share = (1.0 - point.coherence()) * 0.5;
                        [
                            0.9_f32.mul_add(anti_phase_share, 0.1),
                            0.38_f32.mul_add(-anti_phase_share, 0.45),
                            0.9_f32.mul_add(-anti_phase_share, 1.0),
                        ]
                    }
                };
                self.projected.push(SpatialParticle {
                    position: [
                        frequency_to_share(frequency, self.settings.frequency_scale) as f32,
                        point.position(),
                    ],
                    spread: spacing as f32 * 0.35,
                    color: color.map(|channel| channel * weight),
                    size: 0.5_f32.mul_add(emphasis, 0.5),
                    emphasis,
                });
            }
            let excess = (self.collected.len() + self.projected.len()).saturating_sub(MAXIMUM_PARTICLES);
            if excess > 0 {
                self.collected.drain(..excess);
            }
            self.collected.extend_from_slice(&self.projected);
        }
        self.pending_order.clear();
    }
}

/// Maps a positive frequency to its axis share. Validated spectral geometry keeps inputs finite and positive.
#[must_use]
pub fn frequency_to_share(frequency: f64, scale: FrequencyScale) -> f64 {
    match scale {
        FrequencyScale::Logarithmic => (frequency / 20.0).log(1000.0),
        FrequencyScale::Mel => {
            let low = (1.0_f64 + 20.0 / 700.0).log10();
            let high = (1.0_f64 + 20_000.0 / 700.0).log10();
            ((1.0 + frequency / 700.0).log10() - low) / (high - low)
        }
    }
}

/// Inverts the selected axis projection for tick placement and independent round-trip tests.
#[must_use]
pub fn share_to_frequency(share: f64, scale: FrequencyScale) -> f64 {
    match scale {
        FrequencyScale::Logarithmic => 20.0 * 1000.0_f64.powf(share),
        FrequencyScale::Mel => {
            let low = (1.0_f64 + 20.0 / 700.0).log10();
            let high = (1.0_f64 + 20_000.0 / 700.0).log10();
            700.0 * (10.0_f64.powf(share.mul_add(high - low, low)) - 1.0)
        }
    }
}

/// Evaluates the reference contrast S-curve, with the additional high-frequency contrast allowance.
#[must_use]
pub fn contrast_curve(share: f32, contrast: f32) -> f32 {
    let share = share.clamp(0.0, 1.0);
    let contrast = contrast.clamp(0.0, 1.25);
    let exponent = 3.0_f32.mul_add(contrast, 1.0);
    let pivot = 0.2_f32.mul_add(contrast, 0.5);
    let rising = share.powf(exponent);
    let falling = (pivot * (1.0 - share) / (1.0 - pivot)).powf(exponent);
    rising / (rising + falling)
}

/// Computes the display-only 3 dB-per-octave tilt without changing source power or reassignment.
fn tilted_decibels(point: SpectralPoint) -> f32 {
    3.0_f32.mul_add(
        (point.frequency() / 1000.0).log2(),
        10.0 * point.power().max(1.0e-12).log10(),
    )
}

/// Decodes an encoded sRGB palette channel into linear additive light.
fn srgb_to_linear(value: f32) -> f32 {
    if value <= 0.040_45 {
        value / 12.92
    } else {
        ((value + 0.055) / 1.055).powf(2.4)
    }
}

/// Interpolates reference palette stops in linear light on a logarithmic frequency axis.
fn frequency_color(frequency: f64) -> [f32; 3] {
    let mut previous = FREQUENCY_STOPS.first().copied().unwrap_or((20.0, [0.0; 3]));
    for &(upper_frequency, upper) in FREQUENCY_STOPS.iter().skip(1) {
        if frequency <= upper_frequency {
            let (lower_frequency, lower) = previous;
            let position = (frequency / lower_frequency).log(upper_frequency / lower_frequency) as f32;
            let lower = lower.map(srgb_to_linear);
            let upper = upper.map(srgb_to_linear);
            return std::array::from_fn(|channel| {
                let low = lower.get(channel).copied().unwrap_or_default();
                (upper.get(channel).copied().unwrap_or_default() - low).mul_add(position, low)
            });
        }
        previous = (upper_frequency, upper);
    }
    previous.1.map(srgb_to_linear)
}
