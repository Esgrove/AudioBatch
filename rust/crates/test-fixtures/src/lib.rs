//! Locates the committed audio fixtures shared by the C++ and Rust test suites.
//!
//! The fixtures live in the `fixtures` directory at the repository root, shared with the C++ project.
//! Every tone is a one second 1 kHz sine with a peak amplitude of 0.5,
//! which reads -6.02 dBFS and -6.02 LUFS integrated, also for mono files measured as dual mono.
//! This is the only place that knows where the fixtures are,
//! so moving them changes a single path.

use std::path::{Path, PathBuf};

use tempfile::TempDir;

/// Peak level of the fixture tones, which have a peak amplitude of 0.5, in dBFS.
pub const FIXTURE_PEAK_DECIBELS: f64 = -6.0206;

/// Integrated loudness of a fixture tone, in LUFS.
/// A 1 kHz sine on both channels measures the same as its peak level, as in EBU Tech 3341.
pub const FIXTURE_LOUDNESS: f64 = FIXTURE_PEAK_DECIBELS;

/// File name of the intersample fixture, a quarter sample rate sine whose samples fall between its peaks.
///
/// Its sample peak, true peak, and loudness all differ, so a measurement reported in the wrong field fails.
pub const INTERSAMPLE_FIXTURE: &str = "intersample_stereo_44100_24.wav";

/// Sample peak of the intersample fixture, in dBFS.
pub const INTERSAMPLE_PEAK_DECIBELS: f64 = -9.03;

/// True peak of the intersample fixture, in dBTP.
pub const INTERSAMPLE_TRUE_PEAK_DECIBELS: f64 = -5.92;

/// Integrated loudness of the intersample fixture, in LUFS, raised by the K-weighting at 11 kHz.
pub const INTERSAMPLE_LOUDNESS: f64 = -2.67;

/// File names of the Serato fixtures, a swelling tone with a synthetic rainbow Serato Overview tag.
/// Each file holds the same tag in another container Serato writes it to.
///
/// The MP3 and AIFF files hold it as a `GEOB` frame in an `ID3v2` tag,
/// and the FLAC file as a base64 `SERATO_OVERVIEW` Vorbis comment.
/// Its 240 overview slices form six bands of 40: red, yellow, green, cyan, blue, and magenta.
/// Each slice also has background cells and one reserved palette index, which colouring skips.
pub const SERATO_OVERVIEW_FIXTURES: [&str; 3] = [
    "serato_rainbow_stereo_44100_128k.mp3",
    "serato_rainbow_stereo_44100_16.aif",
    "serato_rainbow_stereo_44100_16.flac",
];

/// Number of slices in the Serato Overview of the Serato fixtures.
pub const SERATO_OVERVIEW_SLICES: usize = 240;

/// Colours of the bands in the Serato Overview of the Serato fixtures, as `0xRRGGBB` values.
pub const SERATO_OVERVIEW_BANDS: [u32; 6] = [0xFF_0000, 0xFF_FF00, 0x00_FF00, 0x00_FFFF, 0x00_00FF, 0xFF_00FF];

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
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../fixtures")
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
    fn every_named_fixture_exists() {
        for tone in TONE_FIXTURES {
            assert!(tone.path().is_file(), "Missing fixture {}", tone.file_name);
        }
        for name in SERATO_OVERVIEW_FIXTURES {
            assert!(fixture(name).is_file(), "Missing fixture {name}");
        }
        assert!(fixture(INTERSAMPLE_FIXTURE).is_file());
    }

    #[test]
    fn lossy_fixtures_get_the_wider_tolerance() {
        let lossy = TONE_FIXTURES.iter().find(|tone| tone.lossy).expect("lossy fixture");
        let lossless = TONE_FIXTURES.iter().find(|tone| !tone.lossy).expect("lossless fixture");
        assert!(lossy.tolerance() > lossless.tolerance());
    }
}
