//! Display formatting of analysis values for the GUI and the CLI.
//!
//! Formats levels, audio properties, duration, decimal file size, folders, musical keys, and status.
//! The CLI table uses the same text as the C++ CLI, so `scripts/parity.sh` can compare the two.

use std::error::Error;
use std::path::Path;

use crate::decode::is_mp3_path;
use crate::record::{AnalysisRecord, AnalysisStatus, is_silent_loudness};

/// Lowest level shown in decibels, used for amplitudes just above silence.
const MINIMUM_DISPLAY_DECIBELS: f64 = -100.0;

/// Formats a finite source ratio or coefficient, using a dash when undefined.
#[must_use]
pub fn format_metric(value: Option<f64>, signed: bool) -> String {
    let Some(value) = value.filter(|value| value.is_finite()) else {
        return "-".into();
    };
    let rounded = (value * 100.0).round() / 100.0;
    let normalized = if rounded.abs() < f64::EPSILON { 0.0 } else { rounded };
    if signed {
        format!("{normalized:+.2}")
    } else {
        format!("{normalized:.2}")
    }
}

/// Formats each signed channel mean in percent full scale.
#[must_use]
pub fn format_dc_offsets(metrics: &crate::signal_metrics::SignalMetrics) -> String {
    if metrics.dc_offsets.is_empty() {
        return "-".into();
    }
    metrics
        .dc_offsets
        .iter()
        .enumerate()
        .map(|(channel, value)| format!("Ch {}: {:+.4}%", channel + 1, value * 100.0))
        .collect::<Vec<_>>()
        .join(", ")
}

/// Formats source clip sample counts by channel, including valid zero counts.
#[must_use]
pub fn format_clip_counts(metrics: &crate::signal_metrics::SignalMetrics) -> String {
    if metrics.clip_counts.is_empty() {
        return "-".into();
    }
    metrics
        .clip_counts
        .iter()
        .enumerate()
        .map(|(channel, value)| format!("Ch {}: {value}", channel + 1))
        .collect::<Vec<_>>()
        .join(", ")
}

/// Converts a linear amplitude to decibels, clamped to the given floor.
/// Silence returns the floor instead of negative infinity.
#[must_use]
pub fn gain_to_decibels(gain: f64, minimum_decibels: f64) -> f64 {
    if gain > 0.0 {
        (20.0 * gain.log10()).max(minimum_decibels)
    } else {
        minimum_decibels
    }
}

/// Formats a sample peak without a unit, for compact CLI columns.
#[must_use]
pub fn format_peak_compact(peak: f64) -> String {
    format_amplitude(peak.abs(), "")
}

/// Formats a true peak without a unit, for compact CLI columns.
#[must_use]
pub fn format_true_peak_compact(true_peak: f64) -> String {
    format_amplitude(true_peak.abs(), "")
}

/// Formats a loudness value without a unit, for compact CLI columns.
#[must_use]
pub fn format_loudness_compact(loudness: f64) -> String {
    format_loudness(loudness, "")
}

/// Formats the per-file gain in dB with an explicit sign and without a unit, or an empty string without one.
#[must_use]
pub fn format_custom_gain_compact(record: &AnalysisRecord) -> String {
    record.custom_gain_db.map_or_else(String::new, |decibels| {
        let rounded = (decibels * 10.0).round() / 10.0 + 0.0;
        format!("{rounded:+.1}")
    })
}

/// Returns the average file bitrate in kbps, or 0 when it cannot be derived.
#[must_use]
pub fn average_bitrate_kbps(record: &AnalysisRecord) -> f64 {
    if record.duration_seconds <= 0.0 || record.file_size == 0 {
        return 0.0;
    }
    record.file_size as f64 * 8.0 / record.duration_seconds / 1000.0
}

/// Formats the source bit depth, "Unknown" for MP3 files without one, or "-" when unavailable.
#[must_use]
pub fn format_bits_per_sample(record: &AnalysisRecord) -> String {
    if record.bits_per_sample == 0 {
        return if is_mp3_path(&record.path) { "Unknown" } else { "-" }.to_string();
    }
    record.bits_per_sample.to_string()
}

/// Formats the average bitrate with the kbps unit, or "-" when unavailable.
#[must_use]
pub fn format_bitrate(record: &AnalysisRecord) -> String {
    with_unit(format_bitrate_compact(record), " kbps")
}

/// Formats the average bitrate in kbps without a unit, for columns whose header names the unit.
#[must_use]
pub fn format_bitrate_compact(record: &AnalysisRecord) -> String {
    let bitrate = average_bitrate_kbps(record);
    if bitrate <= 0.0 {
        return String::from("-");
    }
    format!("{}", bitrate.round())
}

/// Formats the sample rate with the kHz unit, or "-" when unavailable.
#[must_use]
pub fn format_sample_rate(record: &AnalysisRecord) -> String {
    with_unit(format_sample_rate_compact(record), " kHz")
}

/// Formats the sample rate in kilohertz without a unit, for columns whose header names the unit.
#[must_use]
pub fn format_sample_rate_compact(record: &AnalysisRecord) -> String {
    match record.sample_rate {
        0 => String::from("-"),
        rate => format_kilohertz(rate),
    }
}

/// Formats a rate in hertz as kilohertz without a unit, whole when it is, for example "48" or "44.1".
#[must_use]
pub fn format_kilohertz(hertz: u32) -> String {
    if hertz.is_multiple_of(1000) {
        format!("{}", hertz / 1000)
    } else {
        format!("{:.1}", f64::from(hertz) / 1000.0)
    }
}

/// Returns the short file type label: the uppercase extension, then the format name, then "Unknown".
#[must_use]
pub fn format_type_label(record: &AnalysisRecord) -> String {
    if let Some(extension) = record
        .path
        .extension()
        .and_then(|extension| extension.to_str())
        .filter(|extension| !extension.is_empty())
    {
        return extension.to_ascii_uppercase();
    }
    let name = record.format_name.trim();
    let name = name.strip_suffix(" file").unwrap_or(name).trim_end();
    if name.is_empty() {
        String::from("Unknown")
    } else {
        name.to_string()
    }
}

/// Formats a duration as minutes and seconds, with hours for long files.
#[must_use]
pub fn format_duration(seconds: f64) -> String {
    if !seconds.is_finite() || seconds <= 0.0 {
        return String::from("-");
    }
    let total = seconds.round() as u64;
    let hours = total / 3600;
    let minutes = (total % 3600) / 60;
    let remaining_seconds = total % 60;
    if hours > 0 {
        format!("{hours}:{minutes:02}:{remaining_seconds:02}")
    } else {
        format!("{minutes}:{remaining_seconds:02}")
    }
}

/// Formats a duration rounded to the nearest tenth as "m:ss.t", or "h:mm:ss.t" from one hour on.
/// Returns "-" when the duration is unknown.
#[must_use]
pub fn format_duration_tenths(seconds: f64) -> String {
    const TENTHS_PER_SECOND: u64 = 10;
    if !seconds.is_finite() || seconds <= 0.0 {
        return String::from("-");
    }
    let tenths = (seconds * TENTHS_PER_SECOND as f64).round() as u64;
    let total_seconds = tenths / TENTHS_PER_SECOND;
    let hours = total_seconds / 3600;
    let minutes = (total_seconds % 3600) / 60;
    let remaining_seconds = total_seconds % 60;
    let fraction = tenths % TENTHS_PER_SECOND;
    if hours > 0 {
        format!("{hours}:{minutes:02}:{remaining_seconds:02}.{fraction}")
    } else {
        format!("{minutes}:{remaining_seconds:02}.{fraction}")
    }
}

/// Formats decimal file size in KB below one MB, or MB otherwise, with one decimal place.
/// Returns "-" for an unknown size.
#[must_use]
pub fn format_file_size(record: &AnalysisRecord) -> String {
    const BYTES_PER_KILOBYTE: u64 = 1000;
    const BYTES_PER_MEGABYTE: u64 = 1_000_000;
    match record.file_size {
        0 => String::from("-"),
        size if size < BYTES_PER_MEGABYTE => format!("{:.1} KB", size as f64 / BYTES_PER_KILOBYTE as f64),
        size => format!("{:.1} MB", size as f64 / BYTES_PER_MEGABYTE as f64),
    }
}

/// Formats the parent folder of a file for the Folder column, without the file name.
/// On macOS a folder inside a user's home folder starts with `~`, see `format_folder_text`.
#[must_use]
pub fn format_folder(path: &Path) -> String {
    let folder = path
        .parent()
        .map_or_else(String::new, |parent| parent.to_string_lossy().into_owned());
    format_folder_text(&folder, cfg!(target_os = "macos"))
}

/// Shortens a folder inside a macOS user home, so `/Users/<user>/Music` shows as `~/Music`.
///
/// Any user's home is shortened, not only the current user's, because a listed folder can belong to another account.
/// `/Users/Shared` is not a home folder and stays absolute, as do all other folders and every folder on Windows.
/// `fixtures/folder_display.tsv` holds the expectations shared with the C++ application.
#[must_use]
pub fn format_folder_text(folder: &str, macos: bool) -> String {
    const USERS_PREFIX: &str = "/Users/";
    const SHARED_FOLDER: &str = "Shared";
    let home_relative = folder
        .strip_prefix(USERS_PREFIX)
        .filter(|_| macos)
        .map(|rest| rest.split_once('/').unwrap_or((rest, "")))
        .filter(|(user, _)| !user.is_empty() && *user != SHARED_FOLDER);
    match home_relative {
        Some((_, "")) => String::from("~"),
        Some((_, inside)) => format!("~/{inside}"),
        None => folder.to_string(),
    }
}

/// Maps a major or minor key, Camelot code, or Open Key code to canonical Camelot notation.
/// Parsing ignores case and surrounding whitespace, without changing the original display spelling.
#[must_use]
pub fn camelot_key(key: &str) -> Option<String> {
    const MINOR_NUMBERS: [u8; 12] = [5, 12, 7, 2, 9, 4, 11, 6, 1, 8, 3, 10];
    const MAJOR_NUMBERS: [u8; 12] = [8, 3, 10, 5, 12, 7, 2, 9, 4, 11, 6, 1];

    let key = key.trim().to_lowercase();
    let tonic = key.chars().next()?;
    if tonic.is_ascii_digit() {
        return camelot_number(&key);
    }
    let pitch_class: usize = match tonic {
        'c' => 0,
        'd' => 2,
        'e' => 4,
        'f' => 5,
        'g' => 7,
        'a' => 9,
        'b' => 11,
        _ => return None,
    };
    let remainder = key.strip_prefix(tonic)?.trim();
    let (offset, mode) = [("#", 1), ("♯", 1), ("sharp", 1), ("b", 11), ("♭", 11), ("flat", 11)]
        .into_iter()
        .find_map(|(accidental, offset)| remainder.strip_prefix(accidental).map(|mode| (offset, mode.trim())))
        .unwrap_or((0, remainder));
    let pitch_class = (pitch_class + offset) % 12;
    match mode {
        "m" | "min" | "minor" => Some(format!("{}A", MINOR_NUMBERS.get(pitch_class)?)),
        "" | "maj" | "major" => Some(format!("{}B", MAJOR_NUMBERS.get(pitch_class)?)),
        _ => None,
    }
}

/// Returns the human-readable status text.
#[must_use]
pub const fn format_status(status: AnalysisStatus) -> &'static str {
    match status {
        AnalysisStatus::Cached => "Cached",
        AnalysisStatus::Analyzed => "Analyzed",
        AnalysisStatus::Failed => "Failed",
        AnalysisStatus::Pending => "Pending",
    }
}

/// Formats a contextual error and its causes at a diagnostic reporting boundary.
/// The contextual error's display text must not already include its source.
pub(crate) fn format_error_chain(mut error: &dyn Error) -> String {
    let mut message = error.to_string();
    while let Some(source) = error.source() {
        message.push_str(": ");
        message.push_str(&source.to_string());
        error = source;
    }
    message
}

/// Parses numbered Camelot and Open Key notation, allowing one leading zero and whitespace before the suffix.
fn camelot_number(key: &str) -> Option<String> {
    let suffix = key.chars().next_back()?;
    let digits = key.strip_suffix(suffix)?.trim_end();
    let digits = digits.strip_prefix('0').unwrap_or(digits);
    if digits.is_empty()
        || digits.len() > 2
        || digits.starts_with('0')
        || !digits.bytes().all(|byte| byte.is_ascii_digit())
    {
        return None;
    }
    let number = digits.parse::<u8>().ok().filter(|number| (1..=12).contains(number))?;
    match suffix {
        'a' | 'b' => Some(format!("{number}{}", suffix.to_ascii_uppercase())),
        'm' => Some(format!("{}A", ((number + 6) % 12) + 1)),
        'd' => Some(format!("{}B", ((number + 6) % 12) + 1)),
        _ => None,
    }
}

/// Appends a unit to a formatted value, leaving the "-" placeholder for a missing value bare.
fn with_unit(value: String, unit: &str) -> String {
    if value == "-" { value } else { value + unit }
}

/// Converts a linear amplitude into a decibel string, rendering silence as "-INF".
fn format_amplitude(amplitude: f64, unit: &str) -> String {
    if amplitude <= 0.0 {
        return format!("-INF{unit}");
    }
    format!("{:.2}{unit}", gain_to_decibels(amplitude, MINIMUM_DISPLAY_DECIBELS))
}

/// Formats a loudness value, rendering the silence sentinel as "-INF".
fn format_loudness(loudness: f64, unit: &str) -> String {
    if is_silent_loudness(loudness) {
        return format!("-INF{unit}");
    }
    format!("{loudness:.2}{unit}")
}

#[cfg(test)]
mod test_error_formatting {
    use super::*;
    use crate::decode::DecodeError;
    use crate::output_file::OutputValidationError;

    #[test]
    fn a_leaf_error_is_not_repeated() {
        let error = std::io::Error::other("disk unavailable");
        assert_eq!(format_error_chain(&error), "disk unavailable");
    }

    #[test]
    fn context_is_followed_by_its_cause() {
        let error = DecodeError::Open(std::io::Error::other("disk unavailable"));
        assert_eq!(
            format_error_chain(&error),
            "File could not be opened for reading: disk unavailable"
        );
    }

    #[test]
    fn nested_causes_are_reported_in_order() {
        let error = OutputValidationError::Unreadable(DecodeError::Open(std::io::Error::other("disk unavailable")));
        assert_eq!(
            format_error_chain(&error),
            "the output file cannot be read: File could not be opened for reading: disk unavailable"
        );
    }
}

#[cfg(test)]
mod test_level_formatting {
    use super::*;

    use crate::record::NEGATIVE_INFINITY_LOUDNESS;

    #[test]
    fn peaks_format_as_decibels() {
        assert_eq!(format_peak_compact(0.5), "-6.02");
        assert_eq!(format_peak_compact(-0.5), "-6.02");
        assert_eq!(format_peak_compact(0.0), "-INF");
        assert_eq!(format_true_peak_compact(1.0), "0.00");
    }

    #[test]
    fn loudness_formats_with_sentinel() {
        assert_eq!(format_loudness_compact(-14.256), "-14.26");
        assert_eq!(format_loudness_compact(NEGATIVE_INFINITY_LOUDNESS), "-INF");
    }

    #[test]
    fn source_statistics_keep_missing_values_distinct_and_name_every_channel() {
        assert_eq!(format_metric(None, false), "-");
        assert_eq!(format_metric(Some(1.0), true), "+1.00");
        assert_eq!(format_metric(Some(-0.0001), true), "+0.00");
        assert_eq!(format_metric(Some(3.010_299_956_6), false), "3.01");
        let metrics = crate::signal_metrics::SignalMetrics {
            dc_offsets: vec![0.25, -0.5],
            clip_counts: vec![4, 3],
            ..Default::default()
        };
        assert_eq!(format_dc_offsets(&metrics), "Ch 1: +25.0000%, Ch 2: -50.0000%");
        assert_eq!(format_clip_counts(&metrics), "Ch 1: 4, Ch 2: 3");
        assert_eq!(metrics.total_clips(), 7);
        assert_eq!(metrics.maximum_dc_offset(), Some(0.5));
    }

    #[test]
    fn tiny_amplitudes_clamp_to_the_floor() {
        assert_eq!(format_peak_compact(1e-9), "-100.00");
    }

    #[test]
    fn custom_gain_has_a_sign_and_one_decimal() {
        let mut record = AnalysisRecord::default();
        assert_eq!(format_custom_gain_compact(&record), "");
        record.custom_gain_db = Some(1.5);
        assert_eq!(format_custom_gain_compact(&record), "+1.5");
        record.custom_gain_db = Some(-3.04);
        assert_eq!(format_custom_gain_compact(&record), "-3.0");
        record.custom_gain_db = Some(-0.04);
        assert_eq!(format_custom_gain_compact(&record), "+0.0");
        record.custom_gain_db = Some(-0.0);
        assert_eq!(format_custom_gain_compact(&record), "+0.0");
    }
}

#[cfg(test)]
mod test_property_formatting {
    use super::*;

    /// Returns an analyzed record of the file at the path.
    fn record(path: &str) -> AnalysisRecord {
        AnalysisRecord {
            path: path.into(),
            ..Default::default()
        }
    }

    #[test]
    fn sample_rates_format_in_kilohertz() {
        let mut value = record("a.wav");
        value.sample_rate = 48_000;
        assert_eq!(format_sample_rate(&value), "48 kHz");
        value.sample_rate = 44_100;
        assert_eq!(format_sample_rate(&value), "44.1 kHz");
        value.sample_rate = 0;
        assert_eq!(format_sample_rate(&value), "-");
    }

    #[test]
    fn kilohertz_are_whole_when_they_can_be() {
        assert_eq!(format_kilohertz(48_000), "48");
        assert_eq!(format_kilohertz(44_100), "44.1");
        assert_eq!(format_kilohertz(96_000), "96");
    }

    #[test]
    fn compact_rates_leave_out_the_unit() {
        let mut value = record("a.wav");
        value.sample_rate = 44_100;
        value.file_size = 40_000;
        value.duration_seconds = 1.0;
        assert_eq!(format_sample_rate_compact(&value), "44.1");
        assert_eq!(format_bitrate_compact(&value), "320");
        value.sample_rate = 96_000;
        value.duration_seconds = 0.0;
        assert_eq!(format_sample_rate_compact(&value), "96");
        assert_eq!(format_bitrate_compact(&value), "-");
    }

    #[test]
    fn bitrate_derives_from_size_and_duration() {
        let mut value = record("a.mp3");
        value.file_size = 40_000;
        value.duration_seconds = 1.0;
        assert_eq!(format_bitrate(&value), "320 kbps");
        value.duration_seconds = 0.0;
        assert_eq!(format_bitrate(&value), "-");
    }

    #[test]
    fn bit_depth_reports_unknown_for_mp3() {
        assert_eq!(format_bits_per_sample(&record("a.mp3")), "Unknown");
        assert_eq!(format_bits_per_sample(&record("a.m4a")), "-");
        let mut value = record("a.wav");
        value.bits_per_sample = 24;
        assert_eq!(format_bits_per_sample(&value), "24");
    }

    #[test]
    fn type_label_uses_extension_then_format_name() {
        assert_eq!(format_type_label(&record("a.flac")), "FLAC");
        let mut value = record("noextension");
        value.format_name = String::from("WAV file");
        assert_eq!(format_type_label(&value), "WAV");
        assert_eq!(format_type_label(&record("noextension")), "Unknown");
    }

    #[test]
    fn durations_format_as_clock_time() {
        assert_eq!(format_duration(61.4), "1:01");
        assert_eq!(format_duration(3725.0), "1:02:05");
        assert_eq!(format_duration(0.0), "-");
        assert_eq!(format_status(AnalysisStatus::Cached), "Cached");
    }

    #[test]
    fn duration_tenths_round_across_minute_and_hour_boundaries() {
        for (seconds, expected) in [
            (205.4, "3:25.4"),
            (3725.0, "1:02:05.0"),
            (0.01, "0:00.0"),
            (0.05, "0:00.1"),
            (59.95, "1:00.0"),
            (3599.95, "1:00:00.0"),
        ] {
            assert_eq!(format_duration_tenths(seconds), expected);
        }
        for seconds in [0.0, -1.0, f64::NAN, f64::INFINITY] {
            assert_eq!(format_duration_tenths(seconds), "-");
        }
    }

    #[test]
    fn file_sizes_use_decimal_units_and_one_decimal_place() {
        let mut value = record("a.flac");
        for (size, expected) in [
            (0, "-"),
            (1, "0.0 KB"),
            (1000, "1.0 KB"),
            (36_200, "36.2 KB"),
            (999_999, "1000.0 KB"),
            (1_000_000, "1.0 MB"),
            (36_200_000, "36.2 MB"),
        ] {
            value.file_size = size;
            assert_eq!(format_file_size(&value), expected);
        }
    }
}

#[cfg(test)]
mod test_camelot_keys {
    use super::*;

    #[test]
    fn keys_match_the_shared_independent_expectations() {
        let cases =
            std::fs::read_to_string(test_fixtures::fixture("camelot_keys.tsv")).expect("reads shared key cases");
        let mut count = 0;
        for line in cases.lines().filter(|line| !line.starts_with('#') && !line.is_empty()) {
            let (input, expected) = line
                .split_once('\t')
                .expect("key case has an input and expected result");
            let expected = (expected != "-").then_some(expected);
            assert_eq!(camelot_key(input).as_deref(), expected, "key {input:?}");
            count += 1;
        }
        assert!(count >= 24, "shared table covers every major and minor pitch class");
    }

    #[test]
    fn enharmonics_and_mode_suffixes_map_without_changing_the_source() {
        for (input, expected) in [
            ("Gm", "6A"),
            ("AM", "8A"),
            ("Abmin", "1A"),
            (" G sharp MINOR ", "1A"),
            ("F♯m", "11A"),
            ("g♭ minor", "11A"),
            ("C flat major", "1B"),
            ("B#", "8B"),
            ("Cbmin", "10A"),
            ("E♯major", "7B"),
        ] {
            assert_eq!(camelot_key(input).as_deref(), Some(expected), "key {input:?}");
        }
    }

    #[test]
    fn numbered_wheels_allow_one_leading_zero_and_whitespace() {
        for (input, expected) in [
            ("08A", "8A"),
            ("012b", "12B"),
            ("1d", "8B"),
            ("8m", "3A"),
            ("12 m", "7A"),
            (" 01 D ", "8B"),
        ] {
            assert_eq!(camelot_key(input).as_deref(), Some(expected), "key {input:?}");
        }
    }

    #[test]
    fn unsupported_and_malformed_keys_are_not_mapped() {
        for input in [
            "", "H", "13A", "0B", "001A", "13d", "Am7", "C##", "Cbb", "1 2A", "+8A", "C dorian",
        ] {
            assert_eq!(camelot_key(input), None, "key {input:?}");
        }
    }
}

#[cfg(test)]
mod test_folder_formatting {
    use super::*;

    #[test]
    fn folders_match_the_shared_independent_expectations() {
        let cases =
            std::fs::read_to_string(test_fixtures::fixture("folder_display.tsv")).expect("reads shared folder cases");
        let mut count = 0;
        for line in cases.lines().filter(|line| !line.starts_with('#') && !line.is_empty()) {
            let mut fields = line.split('\t');
            let (Some(platform), Some(folder), Some(expected), None) =
                (fields.next(), fields.next(), fields.next(), fields.next())
            else {
                panic!("folder case has a platform, a folder, and an expected text: {line:?}");
            };
            assert_eq!(
                format_folder_text(folder, platform == "macos"),
                expected,
                "{platform} folder {folder:?}"
            );
            count += 1;
        }
        assert!(count >= 10, "shared table covers home, shared, and non-home folders");
    }

    #[test]
    fn folder_leaves_out_the_file_name() {
        let expected = if cfg!(target_os = "macos") {
            "~/Music/Album"
        } else {
            "/Users/someone/Music/Album"
        };
        assert_eq!(
            format_folder(Path::new("/Users/someone/Music/Album/track.wav")),
            expected
        );
        assert_eq!(format_folder(Path::new("/tmp/track.wav")), "/tmp");
        assert_eq!(format_folder(Path::new("track.wav")), "");
    }

    #[test]
    fn only_macos_shortens_home_folders() {
        assert_eq!(format_folder_text("/Users/someone/Music", true), "~/Music");
        assert_eq!(
            format_folder_text("/Users/someone/Music", false),
            "/Users/someone/Music"
        );
        assert_eq!(format_folder_text("/Users/", true), "/Users/");
    }
}
