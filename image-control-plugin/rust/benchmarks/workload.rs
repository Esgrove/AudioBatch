//! Deterministic input and normalized sound cases shared by direct DSP and packaged VST3 benchmarks.

use std::f64::consts::TAU;

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
