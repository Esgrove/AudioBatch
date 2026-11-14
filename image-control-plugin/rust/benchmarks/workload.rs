//! Deterministic input and normalized sound cases shared by direct DSP and packaged VST3 benchmarks.

use std::f64::consts::{FRAC_PI_4, TAU};

/// Independent audio content selection, leaving saved-control scenarios unchanged.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum SignalKind {
    Tones,
    MovingNoise,
}

impl SignalKind {
    /// Stable source names keep new broadband measurements separate from historical tone measurements.
    pub const fn name(self) -> &'static str {
        match self {
            Self::Tones => "tones",
            Self::MovingNoise => "moving-noise",
        }
    }

    /// Precomputes immutable audio outside all measured DSP, analysis, and rendering paths.
    pub fn frames(self, sample_rate: f64, channels: usize, count: usize) -> Vec<[f32; 2]> {
        if self == Self::Tones {
            return (0..count)
                .map(|index| input_frame(index, sample_rate, channels))
                .collect();
        }
        let coefficients =
            [90.0, 300.0, 1200.0, 4000.0, 10_000.0].map(|frequency| 1.0 - (-TAU * frequency / sample_rate).exp());
        let mut low_pass = [0.0; 5];
        let mut random = 0x1c20_2601_u32;
        (0..count)
            .map(|index| {
                random = random.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
                let white = f64::from(random >> 8).mul_add(2.0 / 16_777_216.0, -1.0);
                for (value, coefficient) in low_pass.iter_mut().zip(coefficients) {
                    *value = coefficient.mul_add(white - *value, *value);
                }
                let time = index as f64 / sample_rate;
                let mut stereo = [0.0; 2];
                let mut previous = 0.0;
                for (band, (filtered, gain)) in low_pass
                    .into_iter()
                    .chain([white])
                    .zip([16.0, 8.0, 4.0, 2.0, 1.0, 0.6])
                    .enumerate()
                {
                    let noise = filtered - previous;
                    previous = filtered;
                    let ordinal = band as f64;
                    let beat_phase = ordinal.mul_add(0.17, time * ordinal.mul_add(0.5, 2.0)).fract();
                    let envelope = 0.75_f64.mul_add((-8.0 * beat_phase).exp(), 0.25);
                    let pan = 0.92 * ordinal.mul_add(1.1, TAU * time * ordinal.mul_add(0.037, 0.11)).sin();
                    let angle = (pan + 1.0) * FRAC_PI_4;
                    let sample = noise * gain * envelope * 0.06;
                    stereo[0] = sample.mul_add(angle.cos(), stereo[0]);
                    stereo[1] = sample.mul_add(angle.sin(), stereo[1]);
                }
                if channels == 1 {
                    [stereo[0].midpoint(stereo[1]) as f32; 2]
                } else {
                    stereo.map(|value| value as f32)
                }
            })
            .collect()
    }
}

/// Representative saved-control cases, with neutral controls isolated from active imaging paths.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Scenario {
    Neutral,
    Width,
    Pan,
    Gains,
    Recover,
    Delta,
    Bypass,
    Mono,
    Solo,
    MonoInput,
}

impl Scenario {
    /// Enumerates the same sound cases for both benchmark tiers.
    pub const ALL: [Self; 10] = [
        Self::Neutral,
        Self::Width,
        Self::Pan,
        Self::Gains,
        Self::Recover,
        Self::Delta,
        Self::Bypass,
        Self::Mono,
        Self::Solo,
        Self::MonoInput,
    ];

    /// Returns a stable benchmark name, also accepted by the packaged host CLI.
    pub const fn name(self) -> &'static str {
        match self {
            Self::Neutral => "neutral",
            Self::Width => "width",
            Self::Pan => "pan",
            Self::Gains => "gains",
            Self::Recover => "recover",
            Self::Delta => "delta",
            Self::Bypass => "bypass",
            Self::Mono => "mono",
            Self::Solo => "solo",
            Self::MonoInput => "mono-input",
        }
    }

    /// Supplies matching mono-to-mono or stereo-to-stereo bus counts.
    pub const fn channels(self) -> usize {
        if matches!(self, Self::MonoInput) { 1 } else { 2 }
    }

    /// Returns normalized Mid, Side, Width, Recover, Pan, and Solo controls for one band.
    pub fn band(self, band: usize) -> [f64; 6] {
        let mut values = [5.0 / 6.0, 5.0 / 6.0, 0.5, 0.0, 0.5, 0.0];
        let active = matches!(
            self,
            Self::Recover | Self::Delta | Self::Bypass | Self::Mono | Self::Solo | Self::MonoInput
        );
        if matches!(self, Self::Width) || active {
            values[2] = if band.is_multiple_of(2) { 0.2 } else { 0.75 };
        }
        if matches!(self, Self::Pan) || active {
            values[4] = if band.is_multiple_of(2) { 0.3 } else { 0.7 };
        }
        if matches!(self, Self::Gains) || active {
            values[0] = 0.8;
            values[1] = 0.78;
        }
        if active {
            // Keep the measured workload at 80% plain Recover on its 0..200% host range.
            values[3] = 0.4;
        }
        if matches!(self, Self::Solo) && band == 1 {
            values[5] = 1.0;
        }
        values
    }
}

/// Generates bounded, non-silent Mid and Side content without RNG state, files, or output feedback.
/// Binary scaling keeps the independently checked neutral path exact after the input is rounded to f32.
pub fn input_frame(index: usize, sample_rate: f64, channels: usize) -> [f32; 2] {
    let time = index as f64 / sample_rate;
    let mid = (TAU * 997.0 * time)
        .sin()
        .mul_add(0.125, (TAU * 71.0 * time).sin() * 0.0625);
    let side = (TAU * 6037.0 * time)
        .sin()
        .mul_add(0.0625, (TAU * 313.0 * time).cos() * 0.03125);
    if channels == 1 {
        [mid as f32; 2]
    } else {
        [(mid + side) as f32, (mid - side) as f32]
    }
}

#[cfg(test)]
mod signal_tests {
    #[test]
    fn moving_noise_is_repeatable_bounded_and_changes_stereo_balance() {
        let frames = super::SignalKind::MovingNoise.frames(48_000.0, 2, 384_000);
        assert_eq!(frames, super::SignalKind::MovingNoise.frames(48_000.0, 2, 384_000));
        assert!(
            frames
                .iter()
                .flatten()
                .all(|value| value.is_finite() && value.abs() < 1.0)
        );
        let balances: Vec<_> = frames
            .chunks_exact(12_000)
            .map(|chunk| {
                chunk.iter().fold(0.0_f64, |energy, frame| {
                    energy + f64::from(frame[0]).powi(2) - f64::from(frame[1]).powi(2)
                })
            })
            .collect();
        assert!(balances.iter().any(|value| *value > 0.01));
        assert!(balances.iter().any(|value| *value < -0.01));
        assert_ne!(frames.first(), frames.get(48_000));
        let mono = super::SignalKind::MovingNoise.frames(48_000.0, 1, 1000);
        assert!(mono.iter().all(|frame| frame[0].to_bits() == frame[1].to_bits()));
    }
}
