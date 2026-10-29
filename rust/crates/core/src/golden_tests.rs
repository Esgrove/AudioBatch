//! Golden snapshot tests of the analysis and normalization output, using the helpers of the `golden` module.
//!
//! Every test collects all differences before failing, so one run shows everything an implementation changed.

#[cfg(test)]
mod test_golden_signals {
    use crate::golden::{GoldenText, assert_no_differences, measure_signal};
    use crate::signals::{SignalKind, generate};

    /// Channel counts with a special ebur128 path, a generic one, and the surround weighting.
    const CHANNELS: [usize; 6] = [1, 2, 3, 4, 6, 8];

    /// Length of the stereo signals, just over the 3 second short-term window, so it fills completely.
    const SECONDS: f64 = 3.1;

    /// Length of the signals for each channel count, just over one 400 ms gating block.
    const CHANNEL_SECONDS: f64 = 0.45;

    /// Frames per chunk, like a decoder packet, and not a multiple of the block size.
    const CHUNK_FRAMES: usize = 1_000;

    /// Measures every signal kind in stereo at the rate.
    fn every_signal_in_stereo(rate: u32) {
        let mut differences = Vec::new();
        for kind in SignalKind::ALL {
            let frames = (f64::from(rate) * SECONDS) as usize;
            let output = measure_signal(&generate(kind, rate, 2, frames), rate, CHUNK_FRAMES);
            let mut text = GoldenText::default();
            text.meter(&output);
            differences.extend(text.check(&format!("signal/{}_{rate}_2ch", kind.name())));
        }
        assert_no_differences(&differences);
    }

    /// Measures the music-like signal with every channel count at the rate.
    fn every_channel_count(rate: u32) {
        let mut differences = Vec::new();
        for channels in CHANNELS {
            let frames = (f64::from(rate) * CHANNEL_SECONDS) as usize;
            let output = measure_signal(
                &generate(SignalKind::MusicLike, rate, channels, frames),
                rate,
                CHUNK_FRAMES,
            );
            let mut text = GoldenText::default();
            text.meter(&output);
            differences.extend(text.check(&format!("channels/music_{rate}_{channels}ch")));
        }
        assert_no_differences(&differences);
    }

    // The rates go around every change of the true peak oversampling, 4x below 96 kHz, 2x below 192 kHz, then none.

    #[test]
    fn signals_at_8000() {
        every_signal_in_stereo(8_000);
    }

    #[test]
    fn signals_at_22050() {
        every_signal_in_stereo(22_050);
    }

    #[test]
    fn signals_at_44100() {
        every_signal_in_stereo(44_100);
    }

    #[test]
    fn signals_at_48000() {
        every_signal_in_stereo(48_000);
    }

    #[test]
    fn signals_at_88200() {
        every_signal_in_stereo(88_200);
    }

    #[test]
    fn signals_at_96000() {
        every_signal_in_stereo(96_000);
    }

    #[test]
    fn signals_at_176400() {
        every_signal_in_stereo(176_400);
    }

    #[test]
    fn signals_at_192000() {
        every_signal_in_stereo(192_000);
    }

    #[test]
    fn channel_counts_with_4x_oversampling() {
        every_channel_count(44_100);
    }

    #[test]
    fn channel_counts_with_2x_oversampling() {
        every_channel_count(96_000);
    }

    #[test]
    fn channel_counts_without_oversampling() {
        every_channel_count(192_000);
    }

    #[test]
    fn short_and_partial_signals() {
        let mut differences = Vec::new();
        let rate = 48_000;
        for (name, frames) in [
            ("one_frame", 1),
            ("under_one_block", 4_799),
            ("one_block", 4_800),
            ("one_block_and_one_frame", 4_801),
            ("under_gating_block", 19_199),
            ("one_gating_block", 19_200),
        ] {
            let output = measure_signal(&generate(SignalKind::Noise, rate, 2, frames), rate, CHUNK_FRAMES);
            let mut text = GoldenText::default();
            text.meter(&output);
            differences.extend(text.check(&format!("short/{name}")));
        }
        assert_no_differences(&differences);
    }

    #[test]
    fn samples_beyond_full_scale_and_non_finite() {
        let mut differences = Vec::new();
        let rate = 44_100;
        let frames = 44_100;
        let mut loud = generate(SignalKind::Sine, rate, 2, frames);
        for plane in &mut loud {
            for sample in plane.iter_mut() {
                *sample *= 6.0;
            }
        }
        let mut non_finite = generate(SignalKind::Sine, rate, 2, frames);
        for (plane, value) in non_finite.iter_mut().zip([f32::NAN, f32::INFINITY]) {
            for sample in plane.iter_mut().skip(20_000).take(3) {
                *sample = value;
            }
        }
        for (name, planes) in [("beyond_full_scale", loud), ("non_finite", non_finite)] {
            let output = measure_signal(&planes, rate, CHUNK_FRAMES);
            let mut text = GoldenText::default();
            text.meter(&output);
            differences.extend(text.check(&format!("edge/{name}")));
        }
        assert_no_differences(&differences);
    }
}

#[cfg(test)]
mod test_golden_files {
    use std::path::Path;

    use crate::golden::{GoldenText, assert_no_differences};
    use crate::test_helpers::analyze;

    /// Returns the golden name of a fixture file, prefixed with its collection.
    fn golden_name(collection: &str, path: &Path) -> String {
        let name = path.file_name().map(|name| name.to_string_lossy()).unwrap_or_default();
        format!("{collection}/{name}")
    }

    /// Analyzes every file and compares its record against its golden file.
    fn check_files(collection: &str, files: &[std::path::PathBuf]) -> Vec<String> {
        files
            .iter()
            .filter_map(|path| {
                let mut text = GoldenText::default();
                text.record(&analyze(path));
                text.check(&golden_name(collection, path))
            })
            .collect()
    }

    #[test]
    fn every_committed_fixture() {
        let files = test_fixtures::fixture_files();
        assert!(files.len() > 20, "found only {} fixtures", files.len());
        assert_no_differences(&check_files("fixtures", &files));
    }

    #[test]
    fn every_normalization_fixture_when_downloaded() {
        let files = test_fixtures::normalization_files();
        if files.is_empty() {
            eprintln!("Skipping, fixtures/normalization needs git lfs pull");
            return;
        }
        assert_no_differences(&check_files("normalization", &files));
    }
}

#[cfg(test)]
mod test_golden_normalization {
    use std::fs;
    use std::path::Path;
    use std::sync::Arc;

    use crate::golden::{GoldenText, assert_no_differences, hash};
    use crate::normalize::{NormalizationMode, NormalizationOptions, normalization_gain, normalize_file};
    use crate::test_helpers::ready_fixture_copy;

    /// Fixtures normalized in both modes, covering 16 and 24 bits, float input, mono, lossy, and three channels.
    const FIXTURES: [&str; 8] = [
        "sine_stereo_44100_16.wav",
        "sine_stereo_48000_24.flac",
        "sine_stereo_44100_32f.wav",
        "sine_mono_48000_24.aif",
        "intersample_stereo_44100_24.wav",
        "sine_stereo_44100_320k.mp3",
        "sine_3ch_44100_16.aif",
        "stereo_split_44100_16.wav",
    ];

    /// Returns the bytes after the `SSND` chunk header of an AIFF file, the sample data alone.
    fn sample_data(path: &Path) -> Vec<u8> {
        let bytes = fs::read(path).expect("reads the output");
        let start = bytes
            .windows(4)
            .position(|window| window == b"SSND")
            .map_or(0, |position| position + 16);
        bytes.get(start..).map(<[u8]>::to_vec).unwrap_or_default()
    }

    /// Normalizes a copy of the fixture and records the gain, the output samples, and the output analysis.
    fn normalized(fixture: &str, mode: NormalizationMode) -> GoldenText {
        let directory = tempfile::tempdir().expect("creates directory");
        let extension = Path::new(fixture)
            .extension()
            .and_then(|extension| extension.to_str())
            .unwrap_or("wav");
        let source = ready_fixture_copy(&directory, fixture, &format!("source.{extension}"));
        let options = NormalizationOptions {
            mode,
            dispose: Arc::new(|path: &Path| fs::remove_file(path).is_ok()),
            ..NormalizationOptions::default()
        };
        let mut text = GoldenText::default();
        let gain = normalization_gain(&source, &options).expect("fixture has a level");
        text.float32("gain", gain);
        let result = normalize_file(&source, &options, &|| false, &mut |_| {});
        assert_eq!(result.error(), None, "{fixture}");
        let output = result.output().expect("output analysis");
        let samples = sample_data(&output.path);
        text.value("sample_data.len", samples.len());
        text.value("sample_data.hash", format!("{:016x}", hash(&samples)));
        text.record(output);
        text
    }

    #[test]
    fn peak_mode() {
        let differences: Vec<String> = FIXTURES
            .iter()
            .filter_map(|fixture| {
                normalized(fixture, NormalizationMode::Peak).check(&format!("normalize/peak_{fixture}"))
            })
            .collect();
        assert_no_differences(&differences);
    }

    #[test]
    fn loudness_mode() {
        let differences: Vec<String> = FIXTURES
            .iter()
            // ebur128 leaves the third channel of three unused, and the fixture keeps its tone there.
            .filter(|fixture| **fixture != "sine_3ch_44100_16.aif")
            .filter_map(|fixture| {
                normalized(fixture, NormalizationMode::Loudness).check(&format!("normalize/loudness_{fixture}"))
            })
            .collect();
        assert_no_differences(&differences);
    }
}
