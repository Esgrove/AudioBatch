//! Locates the committed audio fixtures shared by the C++ and Rust test suites.
//!
//! The fixtures live in the C++ project's `tests/fixtures` directory at the repository root.
//! Every tone is a one second 1 kHz sine with a peak amplitude of 0.5,
//! which reads -6.02 dBFS and -6.02 LUFS integrated, also for mono files measured as dual mono.
//! This is the only place that knows where the fixtures are,
//! so moving them later changes a single path.

use std::path::{Path, PathBuf};

use tempfile::TempDir;

/// Peak level of the fixture tones, which have a peak amplitude of 0.5, in dBFS.
pub const FIXTURE_PEAK_DECIBELS: f64 = -6.0206;

/// Integrated loudness of a fixture tone, in LUFS.
/// A 1 kHz sine on both channels measures the same as its peak level, as in EBU Tech 3341.
pub const FIXTURE_LOUDNESS: f64 = FIXTURE_PEAK_DECIBELS;

/// Allowed level difference for lossless fixtures, in dB or LU.
pub const LOSSLESS_TOLERANCE: f64 = 0.05;

/// Allowed level difference for lossy fixtures, in dB or LU.
pub const LOSSY_TOLERANCE: f64 = 0.5;

/// Expected properties of one fixture tone.
#[derive(Debug, Clone, Copy)]
pub struct ToneFixture {
    /// File name inside the fixtures directory.
    pub file_name: &'static str,
    /// Number of channels.
    pub channels: u32,
    /// Sample rate in hertz.
    pub sample_rate: u32,
    /// Expected source bit depth, or 0 where the format has none to check.
    pub bits_per_sample: u32,
    /// True for lossy formats, which get a wider level tolerance.
    pub lossy: bool,
}

impl ToneFixture {
    /// Returns the allowed level difference for this fixture.
    #[must_use]
    pub const fn tolerance(&self) -> f64 {
        if self.lossy {
            LOSSY_TOLERANCE
        } else {
            LOSSLESS_TOLERANCE
        }
    }

    /// Returns the absolute path of this fixture.
    #[must_use]
    pub fn path(&self) -> PathBuf {
        fixture(self.file_name)
    }
}

/// The tone fixtures and what analysis should report for them.
///
/// MP3 and Ogg Vorbis files report the fixed default of 16 bits, since the formats store no source bit depth.
pub const TONE_FIXTURES: [ToneFixture; 15] = [
    tone("sine_stereo_44100_16.wav", 2, 44_100, 16, false),
    tone("sine_stereo_48000_24.wav", 2, 48_000, 24, false),
    tone("sine_mono_44100_16.wav", 1, 44_100, 16, false),
    tone("sine_mono_48000_24.wav", 1, 48_000, 24, false),
    tone("sine_stereo_44100_32f.wav", 2, 44_100, 32, false),
    tone("sine_stereo_96000_16.wav", 2, 96_000, 16, false),
    tone("sine_mono_24000_16.wav", 1, 24_000, 16, false),
    tone("sine_stereo_44100_16.aif", 2, 44_100, 16, false),
    tone("sine_mono_48000_24.aif", 1, 48_000, 24, false),
    tone("sine_stereo_44100_16.flac", 2, 44_100, 16, false),
    tone("sine_stereo_48000_24.flac", 2, 48_000, 24, false),
    tone("sine_stereo_44100_320k.mp3", 2, 44_100, 16, true),
    tone("sine_joint_stereo_44100_192k.mp3", 2, 44_100, 16, true),
    tone("sine_mono_44100_128k.mp3", 1, 44_100, 16, true),
    tone("sine_stereo_48000.ogg", 2, 48_000, 16, true),
];

/// Returns the absolute path of the fixtures directory.
#[must_use]
pub fn fixtures_directory() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../tests/fixtures")
}

/// Returns the absolute path of the fixture with the given file name.
#[must_use]
pub fn fixture(name: &str) -> PathBuf {
    fixtures_directory().join(name)
}

/// Copies a fixture into the given temporary directory, optionally under a new name, and returns the copy.
/// Tests that modify files work on copies so the committed fixtures never change.
///
/// # Panics
///
/// Panics when the fixture cannot be copied, which fails the calling test.
#[must_use]
pub fn copy_fixture(directory: &TempDir, name: &str, copy_name: Option<&str>) -> PathBuf {
    let destination = directory.path().join(copy_name.unwrap_or(name));
    std::fs::copy(fixture(name), &destination).expect("Failed to copy fixture");
    destination
}

/// Builds a tone fixture entry for the constant table.
const fn tone(
    file_name: &'static str,
    channels: u32,
    sample_rate: u32,
    bits_per_sample: u32,
    lossy: bool,
) -> ToneFixture {
    ToneFixture {
        file_name,
        channels,
        sample_rate,
        bits_per_sample,
        lossy,
    }
}

#[cfg(test)]
mod test_fixture_paths {
    use super::*;

    #[test]
    fn fixtures_directory_exists() {
        assert!(fixtures_directory().is_dir(), "{}", fixtures_directory().display());
    }

    #[test]
    fn every_tone_fixture_exists() {
        for tone in TONE_FIXTURES {
            assert!(tone.path().is_file(), "Missing fixture {}", tone.file_name);
        }
    }

    #[test]
    fn lossy_fixtures_get_the_wider_tolerance() {
        let lossy = TONE_FIXTURES.iter().find(|tone| tone.lossy).expect("lossy fixture");
        let lossless = TONE_FIXTURES.iter().find(|tone| !tone.lossy).expect("lossless fixture");
        assert!(lossy.tolerance() > lossless.tolerance());
    }
}
