//! Result output of `audiobatch-rs`: the aligned table, and the CSV and JSON records.
//!
//! The table columns and the CSV and JSON field names and values are the same as those of the C++ CLI,
//! with null for silent levels, so `scripts/parity.sh` can compare the two directly.

use std::fmt::Write as _;

use anyhow::Result;
use audiobatch_core::format::{
    format_loudness_compact, format_peak_compact, format_status, format_true_peak_compact, gain_to_decibels,
};
use audiobatch_core::record::{AnalysisRecord, NEGATIVE_INFINITY_LOUDNESS, is_silent_loudness};
use colored::Colorize;
use serde::Serialize;

/// Width of the sample peak column of the table.
const PEAK_COLUMN_WIDTH: usize = 7;

/// Width of the true peak column of the table.
const TRUE_PEAK_COLUMN_WIDTH: usize = 7;

/// Width of the integrated loudness column of the table.
const LOUDNESS_COLUMN_WIDTH: usize = 8;

/// Machine-readable fields of one record, shared by the CSV and JSON output.
/// The field order is also the CSV column order.
#[derive(Debug, Clone, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct RecordRow {
    /// Full path of the file.
    pub path: String,
    /// File name.
    pub name: String,
    /// Container format name.
    pub format: String,
    /// Sample rate in hertz.
    pub sample_rate: u32,
    /// Number of channels.
    pub channels: u32,
    /// Source bit depth, or 0.
    pub bits_per_sample: u32,
    /// Duration in seconds, rounded to milliseconds.
    pub duration_seconds: f64,
    /// Sample peak in dBFS, or null for silence.
    pub peak_dbfs: Option<f64>,
    /// True peak in dBTP, or null for silence.
    pub true_peak_dbtp: Option<f64>,
    /// Integrated loudness in LUFS, or null for silence.
    pub integrated_lufs: Option<f64>,
    /// Highest short-term loudness in LUFS, or null for silence.
    pub max_short_term_lufs: Option<f64>,
    /// Lowercase status, for example "analyzed".
    pub status: String,
    /// Error message of a failed record, empty otherwise.
    pub error: String,
}

impl RecordRow {
    /// Builds the output fields of a record.
    #[must_use]
    pub fn from_record(record: &AnalysisRecord) -> Self {
        Self {
            path: record.path_string(),
            name: record.file_name.clone(),
            format: record.format_name.clone(),
            sample_rate: record.sample_rate,
            channels: record.channels,
            bits_per_sample: record.bits_per_sample,
            duration_seconds: round_to(record.duration_seconds, 1000.0),
            peak_dbfs: decibels_or_null(f64::from(record.overall_peak).abs()),
            true_peak_dbtp: decibels_or_null(record.overall_true_peak),
            integrated_lufs: loudness_or_null(record.integrated_lufs),
            max_short_term_lufs: loudness_or_null(record.max_short_term_lufs),
            status: format_status(record.status).to_ascii_lowercase(),
            error: record.error_message.clone(),
        }
    }
}

/// Returns the aligned table of successful records, with a bold header line.
///
/// Failed records are left out, since their errors are reported on stderr.
/// The track column shows file names, or full paths when `show_full_paths` is set.
#[must_use]
pub fn format_table(records: &[AnalysisRecord], show_full_paths: bool) -> String {
    let header = format!(
        "{:>PEAK_COLUMN_WIDTH$}  {:>TRUE_PEAK_COLUMN_WIDTH$}  {:>LOUDNESS_COLUMN_WIDTH$}  TRACK",
        "dBFS", "dBTP", "LUFS-I"
    );
    let mut table = format!("{}\n", header.bold());
    for record in records.iter().filter(|record| !record.has_error()) {
        let label = if show_full_paths {
            record.path_string()
        } else {
            record.file_name.clone()
        };
        let _ = writeln!(
            table,
            "{:>PEAK_COLUMN_WIDTH$}  {:>TRUE_PEAK_COLUMN_WIDTH$}  {:>LOUDNESS_COLUMN_WIDTH$}  {label}",
            format_peak_compact(f64::from(record.overall_peak)),
            format_true_peak_compact(record.overall_true_peak),
            format_loudness_compact(record.integrated_lufs),
        );
    }
    table
}

/// Returns the records as CSV with a header line, one line per record, and a trailing newline.
/// The header is written even without records, so scripts can always parse the columns.
///
/// # Errors
///
/// Returns an error when the CSV writer fails, which only happens for invalid UTF-8.
pub fn format_csv(records: &[AnalysisRecord]) -> Result<String> {
    let mut writer = csv::WriterBuilder::new().has_headers(false).from_writer(Vec::new());
    writer.write_record(CSV_HEADER)?;
    for record in records {
        let row = RecordRow::from_record(record);
        writer.write_record([
            row.path,
            row.name,
            row.format,
            row.sample_rate.to_string(),
            row.channels.to_string(),
            row.bits_per_sample.to_string(),
            csv_number(row.duration_seconds),
            optional_number(row.peak_dbfs),
            optional_number(row.true_peak_dbtp),
            optional_number(row.integrated_lufs),
            optional_number(row.max_short_term_lufs),
            row.status,
            row.error,
        ])?;
    }
    Ok(String::from_utf8(writer.into_inner()?)?)
}

/// Returns the records as a pretty-printed JSON array with a trailing newline.
///
/// # Errors
///
/// Returns an error when serialization fails.
pub fn format_json(records: &[AnalysisRecord]) -> Result<String> {
    let rows: Vec<RecordRow> = records.iter().map(RecordRow::from_record).collect();
    let mut json = serde_json::to_string_pretty(&rows)?;
    json.push('\n');
    Ok(json)
}

/// CSV header fields, in the order of the `RecordRow` fields.
const CSV_HEADER: [&str; 13] = [
    "path",
    "name",
    "format",
    "sampleRate",
    "channels",
    "bitsPerSample",
    "durationSeconds",
    "peakDbfs",
    "truePeakDbtp",
    "integratedLufs",
    "maxShortTermLufs",
    "status",
    "error",
];

/// Converts a linear amplitude to decibels rounded to two decimals, or `None` for silence.
fn decibels_or_null(amplitude: f64) -> Option<f64> {
    (amplitude > 0.0).then(|| round_to(gain_to_decibels(amplitude, NEGATIVE_INFINITY_LOUDNESS), 100.0))
}

/// Rounds a loudness value to two decimals, or returns `None` for the silence sentinel.
fn loudness_or_null(loudness: f64) -> Option<f64> {
    (!is_silent_loudness(loudness)).then(|| round_to(loudness, 100.0))
}

/// Rounds a value to the precision given as a power of ten, for example 100 for two decimals.
fn round_to(value: f64, scale: f64) -> f64 {
    (value * scale).round() / scale
}

/// Formats an optional number for CSV, with an empty field for `None`.
fn optional_number(value: Option<f64>) -> String {
    value.map(csv_number).unwrap_or_default()
}

/// Formats a number for CSV the way the JSON output writes it,
/// so a whole number keeps its ".0" and both formats show the same text.
fn csv_number(value: f64) -> String {
    serde_json::Value::from(value).to_string()
}

#[cfg(test)]
mod test_record_output {
    use super::*;

    use audiobatch_core::AnalysisStatus;

    fn tone_record() -> AnalysisRecord {
        AnalysisRecord {
            path: "/music/tone.wav".into(),
            file_name: String::from("tone.wav"),
            format_name: String::from("WAV file"),
            sample_rate: 44_100,
            channels: 2,
            bits_per_sample: 16,
            duration_seconds: 1.000_49,
            overall_peak: -0.5,
            overall_true_peak: 0.5,
            integrated_lufs: -6.0234,
            max_short_term_lufs: -6.01,
            status: AnalysisStatus::Analyzed,
            ..Default::default()
        }
    }

    fn silent_record() -> AnalysisRecord {
        AnalysisRecord {
            path: "/music/silence.wav".into(),
            file_name: String::from("silence.wav"),
            integrated_lufs: NEGATIVE_INFINITY_LOUDNESS,
            max_short_term_lufs: NEGATIVE_INFINITY_LOUDNESS,
            status: AnalysisStatus::Cached,
            ..Default::default()
        }
    }

    fn failed_record() -> AnalysisRecord {
        AnalysisRecord {
            path: "/music/broken.wav".into(),
            file_name: String::from("broken.wav"),
            error_message: String::from("Unsupported or unreadable audio file"),
            status: AnalysisStatus::Failed,
            ..Default::default()
        }
    }

    #[test]
    fn row_rounds_levels_and_duration() {
        let row = RecordRow::from_record(&tone_record());
        assert_eq!(row.peak_dbfs, Some(-6.02));
        assert_eq!(row.true_peak_dbtp, Some(-6.02));
        assert_eq!(row.integrated_lufs, Some(-6.02));
        assert_eq!(row.duration_seconds, 1.0);
        assert_eq!(row.status, "analyzed");
    }

    #[test]
    fn silence_is_null() {
        let row = RecordRow::from_record(&silent_record());
        assert_eq!(row.peak_dbfs, None);
        assert_eq!(row.integrated_lufs, None);
        assert_eq!(row.status, "cached");
        let json = format_json(&[silent_record()]).expect("json");
        assert!(json.contains("\"peakDbfs\": null"), "{json}");
    }

    #[test]
    fn json_uses_camel_case_fields() {
        let json = format_json(&[tone_record()]).expect("json");
        for field in CSV_HEADER {
            assert!(json.contains(&format!("\"{field}\"")), "missing {field} in {json}");
        }
        assert!(json.ends_with("]\n"));
    }

    #[test]
    fn csv_has_header_and_escapes_fields() {
        let mut record = failed_record();
        record.error_message = String::from("bad, \"quoted\" error");
        let csv = format_csv(&[tone_record(), record]).expect("csv");
        let lines: Vec<&str> = csv.lines().collect();
        assert_eq!(lines.len(), 3);
        assert_eq!(lines[0], CSV_HEADER.join(","));
        assert!(lines[1].contains(",-6.02,"), "{}", lines[1]);
        assert!(lines[2].ends_with("\"bad, \"\"quoted\"\" error\""), "{}", lines[2]);
        assert_eq!(format_csv(&[]).expect("csv").lines().count(), 1);
    }

    #[test]
    fn table_lists_only_successful_records() {
        colored::control::set_override(false);
        let table = format_table(&[tone_record(), failed_record(), silent_record()], false);
        let lines: Vec<&str> = table.lines().collect();
        assert_eq!(lines.len(), 3);
        assert_eq!(lines[0], "   dBFS     dBTP    LUFS-I  TRACK");
        assert_eq!(lines[1], "  -6.02    -6.02     -6.02  tone.wav");
        assert_eq!(lines[2], "   -INF     -INF      -INF  silence.wav");
        let full = format_table(&[tone_record()], true);
        assert!(full.contains("/music/tone.wav"));
    }
}
