//! Fixed metadata expectations for the real loop samples, independently inspected from their container bytes.

use std::path::PathBuf;

use serde::Deserialize;

/// One file's ordinary tags and Serato values, without assuming that different encodings share cue positions.
#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct MetadataFixture {
    /// File name within `fixtures/metadata`.
    pub file: String,
    /// SHA-256 of the unchanged fixture bytes.
    pub sha256: String,
    /// Ordinary track title.
    pub title: String,
    /// Ordinary artist.
    pub artist: String,
    /// Ordinary album.
    pub album: String,
    /// Ordinary genre.
    pub genre: String,
    /// Ordinary comment.
    pub comment: String,
    /// Ordinary release year, absent in the RK6 samples.
    pub year: Option<u16>,
    /// Ordinary BPM text, distinct from the Serato analysis tempo.
    pub bpm: String,
    /// Ordinary musical key, retaining the file's spelling.
    pub key: String,
    /// Major and minor analysis version, absent when there is no dedicated Analysis tag.
    pub analysis: Option<[u8; 2]>,
    /// Serato BPM, automatic gain, and manual gain.
    pub autotags: [f32; 3],
    /// Track colour as `0xRRGGBB`.
    pub track_rgb: u32,
    /// Explicit beatgrid lock stored in Markers2.
    pub bpm_locked: bool,
    /// Cue points in their original tag order.
    pub cues: Vec<MetadataCue>,
    /// Saved loops, including signed positions before the audio starts.
    pub loops: Vec<MetadataLoop>,
    /// Dedicated beatgrid anchors, absent in the MP3 samples.
    pub beatgrid: Option<Vec<MetadataBeatGridMarker>>,
    /// Complete overview checksum and independently decoded sample colours.
    pub overview: MetadataOverview,
}

/// One hot cue's exact on-disk values.
#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct MetadataCue {
    /// Zero-based cue slot.
    pub index: u8,
    /// Position in milliseconds.
    pub position_ms: u32,
    /// Cue colour as `0xRRGGBB`.
    pub rgb: u32,
    /// Original label, empty when the reader may synthesize a timestamp.
    pub name: String,
}

/// One saved loop's exact on-disk values.
#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct MetadataLoop {
    /// Zero-based loop slot.
    pub index: u8,
    /// Signed start position in milliseconds.
    pub start_ms: i32,
    /// Signed end position in milliseconds.
    pub end_ms: i32,
    /// Loop colour as `0xRRGGBB`.
    pub rgb: u32,
    /// Whether Serato locked the loop.
    pub locked: bool,
    /// Original label.
    pub name: String,
}

/// One beatgrid anchor's values.
#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct MetadataBeatGridMarker {
    /// Position in seconds, decoded from the stored big-endian float.
    pub position_seconds: f32,
    /// Beats to the next anchor, zero for the terminal anchor.
    pub beats_until_next: u32,
    /// Terminal BPM, absent on non-terminal anchors.
    pub bpm: Option<f32>,
}

/// Overview checks that preserve every palette byte and verify representative decoded colours.
#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct MetadataOverview {
    /// Number of complete sixteen-cell slices.
    pub slices: usize,
    /// Lowercase hexadecimal FNV-1a checksum of every cell, in slice-major order, excluding the payload header.
    pub fnv1a64: String,
    /// Samples at the start, interior, and end of the raster.
    pub samples: Vec<MetadataOverviewSample>,
}

/// One independently inspected overview slice.
#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct MetadataOverviewSample {
    /// Zero-based slice index.
    pub index: usize,
    /// All sixteen original palette indices.
    pub cells: [u8; 16],
    /// Mean foreground colour as `0xRRGGBB`, absent for background or reserved cells only.
    pub rgb: Option<u32>,
}

impl MetadataFixture {
    /// Returns the absolute path of the original fixture.
    #[must_use]
    pub fn path(&self) -> PathBuf {
        crate::fixtures_directory().join("metadata").join(&self.file)
    }
}

/// Loads the fixed, shared expectations rather than generating expected values with the parser under test.
///
/// # Panics
///
/// Panics when the expectation file is missing or malformed.
#[must_use]
pub fn metadata_fixtures() -> Vec<MetadataFixture> {
    let path = crate::fixtures_directory().join("metadata").join("expected.json");
    let bytes = std::fs::read(&path).expect("reads fixed metadata expectations");
    serde_json::from_slice(&bytes).expect("validates fixed metadata expectations")
}

#[cfg(test)]
mod test_metadata_fixtures {
    use std::collections::BTreeSet;

    use super::*;

    #[test]
    fn every_real_audio_file_has_one_independent_case() {
        let cases = metadata_fixtures();
        let names: BTreeSet<_> = cases.iter().map(|case| case.file.clone()).collect();
        let audio: BTreeSet<_> = crate::audio_files_in(&crate::fixtures_directory().join("metadata"))
            .into_iter()
            .map(|path| {
                path.file_name()
                    .expect("audio file name")
                    .to_string_lossy()
                    .into_owned()
            })
            .collect();
        assert_eq!(cases.len(), 9);
        assert_eq!(names.len(), cases.len(), "duplicate metadata case");
        assert_eq!(names, audio);
    }

    #[test]
    fn real_fixture_bytes_match_the_independent_capture() {
        for case in metadata_fixtures() {
            assert_eq!(
                crate::file_sha256(&case.path()).expect("hashes real fixture"),
                case.sha256,
                "{}",
                case.file
            );
        }
    }

    #[test]
    fn rk6_lock_and_negative_synth_loop_are_explicit() {
        let cases = metadata_fixtures();
        let locked: Vec<_> = cases.iter().filter(|case| case.bpm_locked).collect();
        assert_eq!(locked.len(), 3);
        assert!(locked.iter().all(|case| case.file.starts_with("RK6_")));
        let negative: Vec<_> = cases
            .iter()
            .flat_map(|case| &case.loops)
            .filter(|saved_loop| saved_loop.start_ms < 0)
            .collect();
        assert_eq!(negative.len(), 1);
        assert_eq!(negative[0].start_ms, -5);
        assert_eq!(negative[0].end_ms, 7555);
    }
}
