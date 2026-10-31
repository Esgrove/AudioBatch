//! Exact identities of fixture bytes and planar PCM, independent of platform byte order.

use std::collections::{BTreeMap, BTreeSet};
use std::fmt::Write as _;
use std::fs::{self, File};
use std::io::{self, Read as _};
use std::path::Path;

use colored::{Color, ColoredString, Colorize as _};
use sha2::{Digest as _, Sha256};

/// Compares an input manifest exactly, or captures it when `UPDATE_INPUT_IDENTITIES=1` is set.
/// This switch is independent of the measurement snapshots' `UPDATE_GOLDEN` switch.
///
/// # Errors
///
/// Returns a missing or mismatching manifest, or a capture failure, with a focused difference report.
pub fn check_identity_manifest(path: &Path, actual: &str) -> Result<(), String> {
    if std::env::var("UPDATE_INPUT_IDENTITIES").as_deref() == Ok("1") {
        if let Some(parent) = path.parent() {
            fs::create_dir_all(parent).map_err(|error| format!("{}: {error}", parent.display()))?;
        }
        fs::write(path, actual).map_err(|error| format!("{}: {error}", path.display()))?;
        println!("{} {}", report_label("[CAPTURED]", Color::Yellow), path.display());
        return Ok(());
    }
    let expected = fs::read_to_string(path).map_err(|error| {
        let status = if error.kind() == io::ErrorKind::NotFound {
            "[MISSING]"
        } else {
            "[ERROR]"
        };
        format!("{} {}\n  {error}", report_label(status, Color::Red), path.display())
    })?;
    let report = manifest_report(path, &expected, actual);
    if expected == actual {
        println!("{report}");
        Ok(())
    } else {
        Err(report)
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
/// The bytes are `AudioBatch PCM SHA-256 v1\0`, little-endian sample rate and channel count as `u32`,
/// frame count as `u64`, then every channel's ordered little-endian `f32` bits.
/// Signed zero and subnormal samples are preserved.
/// Decoder packet boundaries are not included.
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

fn report_label(text: &str, color: Color) -> ColoredString {
    if std::env::var_os("NO_COLOR").is_some() {
        text.normal()
    } else {
        text.color(color).bold()
    }
}

fn manifest_entries(text: &str) -> Option<BTreeMap<&str, &str>> {
    let mut entries = BTreeMap::new();
    for line in text.lines() {
        let (name, value) = if let Some(entry) = line.split_once(" = ") {
            entry
        } else {
            let (value, name) = line.split_once("  ")?;
            (name, value)
        };
        if entries.insert(name, value).is_some() {
            return None;
        }
    }
    Some(entries)
}

fn manifest_report(path: &Path, expected: &str, actual: &str) -> String {
    let mut report = format!(
        "{} {}\n",
        report_label("Input identities:", Color::Cyan),
        path.display()
    );
    let mut matched = 0;
    let mut failed = 0;
    if let Some((expected_entries, actual_entries)) = manifest_entries(expected).zip(manifest_entries(actual)) {
        let names: BTreeSet<_> = expected_entries.keys().chain(actual_entries.keys()).collect();
        for name in names {
            match (expected_entries.get(name), actual_entries.get(name)) {
                (Some(old), Some(new)) if old == new => {
                    matched += 1;
                    if expected == actual {
                        let _ = writeln!(report, "  {} {name}", report_label("[MATCH]", Color::Green));
                    }
                }
                (Some(old), Some(new)) => {
                    failed += 1;
                    let _ = writeln!(
                        report,
                        "  {} {name}\n    expected: {old}\n    actual:   {new}",
                        report_label("[MISMATCH]", Color::Red)
                    );
                }
                (Some(old), None) => {
                    failed += 1;
                    let _ = writeln!(
                        report,
                        "  {} {name}\n    expected: {old}",
                        report_label("[MISSING]", Color::Red)
                    );
                }
                (None, Some(new)) => {
                    failed += 1;
                    let _ = writeln!(
                        report,
                        "  {} {name}\n    actual:   {new}",
                        report_label("[UNEXPECTED]", Color::Red)
                    );
                }
                (None, None) => unreachable!("name comes from a manifest"),
            }
        }
    } else if expected == actual {
        for line in actual.lines() {
            matched += 1;
            let _ = writeln!(report, "  {} {line}", report_label("[MATCH]", Color::Green));
        }
    }
    if expected != actual && failed == 0 {
        failed = 1;
        let reason = if expected.lines().eq(actual.lines()) {
            "LINE ENDINGS / FINAL NEWLINE"
        } else {
            "ORDER / FORMATTING / TEXT"
        };
        let offset = expected
            .bytes()
            .zip(actual.bytes())
            .position(|(old, new)| old != new)
            .unwrap_or_else(|| expected.len().min(actual.len()));
        let _ = writeln!(
            report,
            "  {} {reason}; first differing byte: {offset}",
            report_label("[MISMATCH]", Color::Red)
        );
        for (label, text) in [("expected", expected), ("actual", actual)] {
            let _ = writeln!(
                report,
                "    {label}: {} bytes, {} LF, {} CRLF, final newline: {}",
                text.len(),
                text.matches('\n').count() - text.matches("\r\n").count(),
                text.matches("\r\n").count(),
                text.ends_with('\n')
            );
        }
    }
    let status = if failed == 0 {
        report_label("[PASS]", Color::Green)
    } else {
        report_label("[FAIL]", Color::Red)
    };
    let _ = write!(report, "{status} {matched} matched, {failed} failed");
    report
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
            .unwrap_or_else(|report| panic!("{report}"));
    }

    #[test]
    fn mismatch_report_names_only_changed_entries() {
        let report = manifest_report(
            Path::new("checksums.sha256"),
            "same-hash  unchanged.wav\nold-hash  broken.wav\n",
            "same-hash  unchanged.wav\nnew-hash  broken.wav\n",
        );
        assert!(report.contains("broken.wav\n    expected: old-hash\n    actual:   new-hash"));
        assert!(report.contains("1 matched, 1 failed"));
        assert!(!report.contains("unchanged.wav"));
    }

    #[test]
    fn report_distinguishes_missing_and_unexpected_entries() {
        let report = manifest_report(
            Path::new("signals.txt"),
            "missing = 1\nshared = 2\n",
            "shared = 2\nunexpected = 3\n",
        );
        assert!(report.contains("MISSING"));
        assert!(report.contains("missing\n    expected: 1"));
        assert!(report.contains("UNEXPECTED"));
        assert!(report.contains("unexpected\n    actual:   3"));
        assert!(report.contains("1 matched, 2 failed"));
    }

    #[test]
    fn text_only_changes_remain_failures_with_an_explanation() {
        let expected = "first = 1\nsecond = 2\n";
        for actual in ["first = 1\r\nsecond = 2\r\n", "first = 1\nsecond = 2"] {
            let report = manifest_report(Path::new("signals.txt"), expected, actual);
            assert!(report.contains("LINE ENDINGS / FINAL NEWLINE"));
            assert!(report.contains("2 matched, 1 failed"));
        }
        let reordered = manifest_report(Path::new("signals.txt"), expected, "second = 2\nfirst = 1\n");
        assert!(reordered.contains("ORDER / FORMATTING / TEXT"));
        assert!(reordered.contains("2 matched, 1 failed"));
        let duplicate = manifest_report(Path::new("signals.txt"), expected, "first = 1\nfirst = 1\n");
        assert!(duplicate.contains("failed"));
        assert!(!duplicate.contains("0 failed"));
    }

    #[test]
    fn matching_report_lists_entries_and_totals() {
        let text = "first = 1\nsecond = 2\n";
        let report = manifest_report(Path::new("signals.txt"), text, text);
        assert!(report.contains("MATCH"));
        assert!(report.contains("first"));
        assert!(report.contains("second"));
        assert!(report.contains("2 matched, 0 failed"));
        let plain = manifest_report(Path::new("identity.txt"), "expected\n", "expected\n");
        assert!(plain.contains("expected"));
        assert!(plain.contains("1 matched, 0 failed"));
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
