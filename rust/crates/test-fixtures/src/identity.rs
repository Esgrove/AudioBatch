//! Exact identities of fixture bytes and planar PCM, independent of platform byte order.

use std::fmt::Write as _;
use std::fs::{self, File};
use std::io::{self, Read as _};
use std::path::Path;

use sha2::{Digest as _, Sha256};

/// Compares an input manifest exactly, or captures it when `UPDATE_INPUT_IDENTITIES=1` is set.
/// This switch is independent of the measurement snapshots' `UPDATE_GOLDEN` switch.
///
/// # Errors
///
/// Returns a missing or mismatching manifest, or a capture failure, with its path and contents.
pub fn check_identity_manifest(path: &Path, actual: &str) -> Result<(), String> {
    if std::env::var("UPDATE_INPUT_IDENTITIES").as_deref() == Ok("1") {
        if let Some(parent) = path.parent() {
            fs::create_dir_all(parent).map_err(|error| format!("{}: {error}", parent.display()))?;
        }
        return fs::write(path, actual).map_err(|error| format!("{}: {error}", path.display()));
    }
    let expected = fs::read_to_string(path).map_err(|error| format!("{}: {error}", path.display()))?;
    if expected == actual {
        Ok(())
    } else {
        Err(format!(
            "{}: input identity differs\nexpected:\n{expected}\nactual:\n{actual}",
            path.display()
        ))
    }
}

/// Returns the SHA-256 of a file's bytes without decoding or interpreting its metadata.
///
/// # Errors
///
/// Returns a file open or read error.
pub fn file_sha256(path: &Path) -> io::Result<String> {
    let mut file = File::open(path)?;
    let mut hasher = Sha256::new();
    let mut buffer = [0_u8; 16_384];
    loop {
        let count = file.read(&mut buffer)?;
        if count == 0 {
            return Ok(lower_hex(&hasher.finalize()));
        }
        let bytes = buffer
            .get(..count)
            .ok_or_else(|| io::Error::other("invalid file read length"))?;
        hasher.update(bytes);
    }
}

/// Returns the SHA-256 of a versioned, channel-major planar PCM representation.
///
/// The bytes are `AudioBatch PCM SHA-256 v1\0`, little-endian sample rate and channel count
/// as `u32`, frame count as `u64`, then every channel's ordered little-endian `f32` bits.
/// Signed zero and subnormal samples are preserved; decoder packet boundaries are not included.
///
/// # Panics
///
/// Panics for a zero sample rate, no channels, unequal plane lengths, nonfinite samples,
/// or a channel count that cannot fit in `u32`.
#[must_use]
pub fn pcm_sha256(sample_rate: u32, planes: &[Vec<f32>]) -> String {
    assert!(sample_rate > 0, "PCM identity requires a sample rate");
    assert!(!planes.is_empty(), "PCM identity requires channels");
    let frames = planes.first().expect("PCM has channels").len();
    let channels = u32::try_from(planes.len()).expect("PCM channel count fits u32");
    let mut hasher = Sha256::new();
    hasher.update(b"AudioBatch PCM SHA-256 v1\0");
    hasher.update(sample_rate.to_le_bytes());
    hasher.update(channels.to_le_bytes());
    hasher.update((frames as u64).to_le_bytes());
    for plane in planes {
        assert_eq!(plane.len(), frames, "PCM identity requires equal plane lengths");
        for sample in plane {
            assert!(sample.is_finite(), "PCM identity requires finite samples");
            hasher.update(sample.to_bits().to_le_bytes());
        }
    }
    lower_hex(&hasher.finalize())
}

fn lower_hex(bytes: &[u8]) -> String {
    let mut text = String::with_capacity(bytes.len() * 2);
    for byte in bytes {
        let _ = write!(text, "{byte:02x}");
    }
    text
}

#[cfg(test)]
mod test_identity {
    use tempfile::tempdir;

    use super::*;

    #[test]
    fn committed_fixture_bytes_have_shared_identities() {
        let directory = crate::fixtures_directory();
        let mut actual = String::new();
        for (prefix, files) in [
            ("", crate::fixture_files()),
            (
                "plugin_expected/",
                crate::audio_files_in(&directory.join("plugin_expected")),
            ),
        ] {
            assert!(!files.is_empty(), "committed fixture collection is missing: {prefix}");
            for path in files {
                let name = path.file_name().expect("fixture file name").to_string_lossy();
                let _ = writeln!(
                    actual,
                    "{}  {prefix}{name}",
                    file_sha256(&path).expect("hashes fixture")
                );
            }
        }
        check_identity_manifest(&directory.join("checksums.sha256"), &actual)
            .expect("committed fixture bytes match the shared manifest");
    }

    #[test]
    fn missing_and_changed_manifests_fail_closed() {
        let directory = tempdir().expect("temporary directory");
        let path = directory.path().join("identity.txt");
        assert!(check_identity_manifest(&path, "expected\n").is_err());
        fs::write(&path, "expected\n").expect("writes manifest");
        assert!(check_identity_manifest(&path, "expected\n").is_ok());
        assert!(check_identity_manifest(&path, "changed\n").is_err());
    }

    #[test]
    fn pcm_encoding_matches_independent_sha256() {
        let planes = vec![vec![0.25, -0.5], vec![0.0, 1.0]];
        assert_eq!(
            pcm_sha256(48_000, &planes),
            "7851029d7d50c04dffa56eeef4b0168aed6004ae0c17437fea1cd38b6eec9680"
        );
    }

    #[test]
    fn every_sample_bit_and_dimension_affects_identity() {
        let original = vec![vec![0.25, 0.0], vec![-0.5, 1.0]];
        let expected = pcm_sha256(48_000, &original);
        for replacement in [f32::from_bits(0.25_f32.to_bits() + 1), -0.0, f32::from_bits(1)] {
            let mut changed = original.clone();
            *changed
                .first_mut()
                .expect("first channel")
                .first_mut()
                .expect("first sample") = replacement;
            assert_ne!(pcm_sha256(48_000, &changed), expected);
        }
        let mut signed_zero = original.clone();
        *signed_zero
            .first_mut()
            .expect("first channel")
            .get_mut(1)
            .expect("zero sample") = -0.0;
        assert_ne!(pcm_sha256(48_000, &signed_zero), expected);
        assert_ne!(
            pcm_sha256(48_000, &[vec![0.0]]),
            pcm_sha256(48_000, &[vec![f32::from_bits(1)]])
        );
        assert_ne!(
            pcm_sha256(48_000, &[vec![f32::from_bits(1)]]),
            pcm_sha256(48_000, &[vec![f32::from_bits(2)]])
        );
        let mut reordered = original.clone();
        reordered.reverse();
        assert_ne!(pcm_sha256(48_000, &reordered), expected);
        assert_ne!(pcm_sha256(44_100, &original), expected);
        assert_ne!(
            pcm_sha256(48_000, &[original.first().expect("first channel").clone()]),
            expected
        );
        let longer: Vec<_> = original
            .into_iter()
            .map(|mut plane| {
                plane.push(0.0);
                plane
            })
            .collect();
        assert_ne!(pcm_sha256(48_000, &longer), expected);
    }

    #[test]
    fn packet_boundaries_do_not_affect_identity() {
        let planes = vec![vec![0.25, -0.5, f32::from_bits(1), -0.0], vec![1.0, 0.0, -1.0, 0.5]];
        let expected = pcm_sha256(48_000, &planes);
        for chunk_frames in 1..=5 {
            let reconstructed: Vec<_> = planes
                .iter()
                .map(|plane| plane.chunks(chunk_frames).flatten().copied().collect())
                .collect();
            assert_eq!(pcm_sha256(48_000, &reconstructed), expected);
        }
    }

    #[test]
    fn file_identity_matches_standard_vector_and_detects_byte_changes() {
        let directory = tempdir().expect("temporary directory");
        let path = directory.path().join("input.bin");
        fs::write(&path, b"abc").expect("writes input");
        let expected = file_sha256(&path).expect("hashes input");
        assert_eq!(
            expected,
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
        );
        fs::write(&path, b"abd").expect("changes input");
        assert_ne!(file_sha256(&path).expect("hashes changed input"), expected);
    }

    #[test]
    #[should_panic(expected = "equal plane lengths")]
    fn unequal_plane_lengths_are_rejected() {
        let _ = pcm_sha256(48_000, &[vec![0.0], vec![]]);
    }

    #[test]
    #[should_panic(expected = "finite samples")]
    fn nonfinite_samples_are_rejected() {
        let _ = pcm_sha256(48_000, &[vec![f32::from_bits(0x7fc0_0000)]]);
    }
}
