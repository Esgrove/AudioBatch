//! Display formatting of analysis values for the GUI and the CLI.
//!
//! Formats peak, true peak, and loudness levels, bitrate, sample rate, bit depth, file type, duration, and status,
//! following `AudioAnalysisFormatting.cpp` of the C++ application.

use crate::decode::is_mp3_path;
use crate::record::{AnalysisRecord, AnalysisStatus, is_silent_loudness};

/// Lowest level shown in decibels, used for amplitudes just above silence.
const MINIMUM_DISPLAY_DECIBELS: f64 = -100.0;

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

/// Formats a sample peak with the dBFS unit, rendering silence as "-INF dBFS".
#[must_use]
pub fn format_peak_display(peak: f32) -> String {
    format_amplitude(f64::from(peak).abs(), " dBFS")
}

/// Formats a true peak with the dBTP unit.
#[must_use]
pub fn format_true_peak_display(true_peak: f64) -> String {
    format_amplitude(true_peak.abs(), " dBTP")
}

/// Formats a loudness value with the LUFS unit.
#[must_use]
pub fn format_loudness_display(loudness: f64) -> String {
    format_loudness(loudness, " LUFS")
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
    record
        .custom_gain_db
        .map_or_else(String::new, |decibels| format!("{decibels:+.1}"))
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
mod test_level_formatting {
    use super::*;

    use crate::record::NEGATIVE_INFINITY_LOUDNESS;

    #[test]
    fn peaks_format_as_decibels() {
        assert_eq!(format_peak_display(0.5), "-6.02 dBFS");
        assert_eq!(format_peak_display(-0.5), "-6.02 dBFS");
        assert_eq!(format_peak_display(0.0), "-INF dBFS");
        assert_eq!(format_true_peak_compact(1.0), "0.00");
    }

    #[test]
    fn loudness_formats_with_sentinel() {
        assert_eq!(format_loudness_display(-14.256), "-14.26 LUFS");
        assert_eq!(format_loudness_compact(NEGATIVE_INFINITY_LOUDNESS), "-INF");
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
    }
}

#[cfg(test)]
mod test_property_formatting {
    use super::*;

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
        assert_eq!(format_bits_per_sample(&record("a.ogg")), "-");
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
}
