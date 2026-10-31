//! Deterministic in-memory test signals for the measurement tests and benchmarks.
//!
//! Every generator returns planar samples, one vector per channel, and depends only on its arguments,
//! with shared SHA-256 input identities checked independently of the measurement snapshots.
//! The noise comes from a seeded xorshift generator instead of a random crate for the same reason,
//! and the sine is a polynomial of plain multiplications and additions,
//! since the platform math library may round `sin` and `exp` differently in the last bit.
//! The module is public only for the benchmarks, which is why it is hidden from the documentation.

#![allow(
    clippy::suboptimal_flops,
    reason = "separate multiplies and adds keep the signals the same whether or not the target fuses them"
)]

use std::f64::consts::{FRAC_PI_4, PI, TAU};

/// Kinds of generated signal, each exercising another part of the measurement.
#[doc(hidden)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SignalKind {
    /// A 1 kHz sine at half scale, another phase on every channel.
    Sine,
    /// Uniform white noise at half scale from a seeded generator, another seed per channel.
    Noise,
    /// A quarter sample rate sine whose samples fall between its peaks, so the true peak exceeds the sample peak.
    Intersample,
    /// Noise that fades out exponentially into subnormal values and then digital silence.
    FadeToSubnormal,
    /// A constant offset, which the K-weighting removes.
    Dc,
    /// Digital silence.
    Silence,
    /// A full scale 100 Hz square wave.
    Square,
    /// A few amplitude modulated partials, closer to music than a single tone.
    MusicLike,
}

/// Seeded xorshift generator for reproducible noise.
#[doc(hidden)]
#[derive(Debug, Clone)]
pub struct Xorshift {
    state: u64,
}

impl SignalKind {
    /// All signal kinds, in a stable order.
    pub const ALL: [Self; 8] = [
        Self::Sine,
        Self::Noise,
        Self::Intersample,
        Self::FadeToSubnormal,
        Self::Dc,
        Self::Silence,
        Self::Square,
        Self::MusicLike,
    ];

    /// Returns a short name for golden files and benchmark labels.
    #[must_use]
    pub const fn name(self) -> &'static str {
        match self {
            Self::Sine => "sine",
            Self::Noise => "noise",
            Self::Intersample => "intersample",
            Self::FadeToSubnormal => "fade",
            Self::Dc => "dc",
            Self::Silence => "silence",
            Self::Square => "square",
            Self::MusicLike => "music",
        }
    }
}

impl Xorshift {
    /// Creates a generator from a seed, scrambled with the `SplitMix64` finalizer,
    /// so neighbouring seeds start far apart and a zero seed, which would only produce zeros, cannot occur.
    #[must_use]
    pub const fn new(seed: u64) -> Self {
        let mut value = seed.wrapping_add(0x9E37_79B9_7F4A_7C15);
        value = (value ^ (value >> 30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
        value = (value ^ (value >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
        value ^= value >> 31;
        Self {
            state: if value == 0 { 1 } else { value },
        }
    }

    /// Returns the next value of the sequence.
    pub const fn next_u64(&mut self) -> u64 {
        let mut value = self.state;
        value ^= value << 13;
        value ^= value >> 7;
        value ^= value << 17;
        self.state = value;
        value
    }

    /// Returns a value from -1 to 1.
    pub fn next_signed(&mut self) -> f64 {
        (self.next_u64() >> 11) as f64 / (1_u64 << 52) as f64 - 1.0
    }

    /// Returns a value below `limit`, which must be above zero.
    pub const fn next_below(&mut self, limit: u64) -> u64 {
        self.next_u64() % limit
    }
}

/// Generates `frames` frames of the signal at the sample rate, one vector per channel.
#[doc(hidden)]
#[must_use]
pub fn generate(kind: SignalKind, sample_rate: u32, channels: usize, frames: usize) -> Vec<Vec<f32>> {
    (0..channels)
        .map(|channel| generate_channel(kind, sample_rate, channel, frames))
        .collect()
}

/// Generates one channel of the signal.
fn generate_channel(kind: SignalKind, sample_rate: u32, channel: usize, frames: usize) -> Vec<f32> {
    let rate = f64::from(sample_rate);
    let phase = channel as f64 * 0.37;
    let mut noise = Xorshift::new(0x5EED_0000 + channel as u64);
    // Falls by about 520 dB per second, reaching subnormal floats within a fifth of a second.
    let decay = 1.0 - 60.0 / rate;
    let mut envelope = 0.5;
    (0..frames)
        .map(|frame| {
            let time = frame as f64 / rate;
            let value = match kind {
                SignalKind::Sine => 0.5 * sine(TAU * 1000.0 * time + phase),
                SignalKind::Noise => 0.5 * noise.next_signed(),
                SignalKind::Intersample => 0.5 * sine(TAU * 0.25 * frame as f64 + FRAC_PI_4),
                SignalKind::FadeToSubnormal => {
                    envelope *= decay;
                    envelope * noise.next_signed()
                }
                SignalKind::Dc => 0.25,
                SignalKind::Silence => 0.0,
                SignalKind::Square => {
                    if (time * 100.0).fract() < 0.5 {
                        1.0
                    } else {
                        -1.0
                    }
                }
                SignalKind::MusicLike => music_like(time, phase, &mut noise),
            };
            value as f32
        })
        .collect()
}

/// Returns one sample of a few amplitude modulated partials with a little noise.
fn music_like(time: f64, phase: f64, noise: &mut Xorshift) -> f64 {
    let envelope = 0.6 + 0.4 * sine(TAU * 0.5 * time);
    let partials = 0.30 * sine(TAU * 110.0 * time + phase)
        + 0.15 * sine(TAU * 220.0 * time)
        + 0.08 * sine(TAU * 1760.0 * time + phase)
        + 0.04 * sine(TAU * 7040.0 * time);
    envelope * partials + 0.02 * noise.next_signed()
}

/// Returns the sine of an angle in radians from plain arithmetic, the same on every platform.
///
/// Reduces the angle to the range from minus pi to pi and sums the Taylor series to the 23rd power,
/// which is accurate to about 1e-10, plenty for a test signal.
fn sine(angle: f64) -> f64 {
    let turns = ((angle + PI) / TAU).floor();
    let reduced = angle - turns * TAU;
    let square = reduced * reduced;
    let mut term = reduced;
    let mut sum = reduced;
    for power in (3..=23_u32).step_by(2) {
        term *= -square / f64::from(power * (power - 1));
        sum += term;
    }
    sum
}
