//! Golden snapshots that freeze the exact output of the measurement and normalization code.
//!
//! The level tests elsewhere check values within decibel tolerances, which a change in the last bit passes.
//! These tests record every value with its exact bits in a small text file per input in `tests/golden/`,
//! as `f64::to_bits` hex next to the decimal value for reading,
//! and fail when any value differs, printing the old and the new line.
//! Running the tests with `UPDATE_GOLDEN=1` rewrites the files.
//! Only regenerate them in a commit that explains why the output changed, never inside an optimization.
//! The inputs are the committed fixtures, the Git LFS fixtures in `fixtures/normalization` when present,
//! and in-memory signals at every rate where the true peak oversampling changes.

use std::fmt::Write as _;
use std::fs;
use std::hash::Hasher as _;
use std::path::{Path, PathBuf};

use crate::cache::serialize_history;
use crate::meter::{BlockMeter, MeterOutput};
use crate::record::AnalysisRecord;

/// Environment variable that rewrites the golden files instead of comparing against them.
const UPDATE_VARIABLE: &str = "UPDATE_GOLDEN";

/// Builds the text of one golden file, a line per value.
#[derive(Default)]
pub struct GoldenText {
    text: String,
}

/// Stable 64-bit FNV-1a hash, the same on every platform and Rust version, unlike the standard hasher.
struct Fnv1a(u64);

impl GoldenText {
    /// Returns the text built so far.
    pub fn text(&self) -> &str {
        &self.text
    }

    /// Adds a 64-bit float with its exact bits.
    pub fn float(&mut self, name: &str, value: f64) {
        let _ = writeln!(self.text, "{name} = {value:?} [{:016x}]", value.to_bits());
    }

    /// Adds a 32-bit float with its exact bits.
    pub fn float32(&mut self, name: &str, value: f32) {
        let _ = writeln!(self.text, "{name} = {value:?} [{:08x}]", value.to_bits());
    }

    /// Adds any value through its debug format.
    pub fn value(&mut self, name: &str, value: impl std::fmt::Debug) {
        let _ = writeln!(self.text, "{name} = {value:?}");
    }

    /// Adds the length and a stable hash of a byte blob.
    pub fn blob(&mut self, name: &str, bytes: &[u8]) {
        let _ = writeln!(self.text, "{name} = {} bytes [{:016x}]", bytes.len(), hash(bytes));
    }

    /// Adds every value of a series of 32-bit floats with their exact bits, eight per line.
    pub fn series(&mut self, name: &str, values: &[f32]) {
        let _ = writeln!(self.text, "{name}.len = {}", values.len());
        for (line, chunk) in values.chunks(8).enumerate() {
            let bits: Vec<String> = chunk.iter().map(|value| format!("{:08x}", value.to_bits())).collect();
            let _ = writeln!(self.text, "{name}[{}] = {}", line * 8, bits.join(" "));
        }
    }

    /// Adds every field the measurement fills in an analysis record, including the cache blobs.
    pub fn record(&mut self, record: &AnalysisRecord) {
        self.value("status", record.status);
        self.value("error", record.error);
        self.value("error_message", &record.error_message);
        self.value("format_name", &record.format_name);
        self.value("sample_rate", record.sample_rate);
        self.value("channels", record.channels);
        self.value("bits_per_sample", record.bits_per_sample);
        self.value("length_in_samples", record.length_in_samples);
        self.float("duration_seconds", record.duration_seconds);
        self.float32("peak_left", record.peak_left);
        self.float32("peak_right", record.peak_right);
        self.float32("overall_peak", record.overall_peak);
        self.float("true_peak_left", record.true_peak_left);
        self.float("true_peak_right", record.true_peak_right);
        self.float("overall_true_peak", record.overall_true_peak);
        self.float("integrated_lufs", record.integrated_lufs);
        self.float("max_short_term_lufs", record.max_short_term_lufs);
        if let Some(history) = &record.short_term_loudness {
            self.value("history.samples_per_point", history.samples_per_point);
            self.blob("history.blob", &serialize_history(history, record).unwrap_or_default());
            self.series("history", &history.values);
        }
        if let Some(waveform) = &record.waveform {
            self.blob("waveform.blob", &waveform.to_bytes());
        }
    }

    /// Adds every value the block meter measured.
    pub fn meter(&mut self, output: &MeterOutput) {
        for (channel, value) in output.min_samples.iter().enumerate() {
            self.float32(&format!("min_sample[{channel}]"), *value);
        }
        for (channel, value) in output.max_samples.iter().enumerate() {
            self.float32(&format!("max_sample[{channel}]"), *value);
        }
        for (channel, value) in output.true_peaks.iter().enumerate() {
            self.float(&format!("true_peak[{channel}]"), *value);
        }
        self.float("integrated_lufs", output.integrated_lufs);
        self.float("max_short_term_lufs", output.max_short_term_lufs);
        self.value("samples_per_point", output.samples_per_point);
        self.blob("waveform.blob", &output.waveform.to_bytes());
        self.series("history", &output.history);
    }

    /// Compares the text against the golden file of the name, or rewrites it when `UPDATE_GOLDEN` is set.
    /// Returns a description of the differences, or `None` when the file matches.
    pub fn check(&self, name: &str) -> Option<String> {
        let path = golden_path(name);
        if std::env::var_os(UPDATE_VARIABLE).is_some() {
            if let Some(parent) = path.parent() {
                fs::create_dir_all(parent).expect("creates the golden directory");
            }
            fs::write(&path, &self.text).expect("writes the golden file");
            return None;
        }
        let Ok(expected) = fs::read_to_string(&path) else {
            return Some(format!(
                "{name}: no golden file at {}, run with {UPDATE_VARIABLE}=1 to create it",
                path.display()
            ));
        };
        difference(name, &expected, &self.text)
    }
}

impl Fnv1a {
    /// Starts a hash with the FNV offset basis.
    const fn new() -> Self {
        Self(0xcbf2_9ce4_8422_2325)
    }
}

impl std::hash::Hasher for Fnv1a {
    fn finish(&self) -> u64 {
        self.0
    }

    fn write(&mut self, bytes: &[u8]) {
        for &byte in bytes {
            self.0 ^= u64::from(byte);
            self.0 = self.0.wrapping_mul(0x0100_0000_01b3);
        }
    }
}

/// Returns the stable hash of a byte blob.
pub fn hash(bytes: &[u8]) -> u64 {
    let mut hasher = Fnv1a::new();
    hasher.write(bytes);
    hasher.finish()
}

/// Returns the path of the golden file of the name.
fn golden_path(name: &str) -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("tests/golden")
        .join(format!("{name}.txt"))
}

/// Describes the lines that differ between the expected and the actual text, or returns `None` when they match.
fn difference(name: &str, expected: &str, actual: &str) -> Option<String> {
    if expected == actual {
        return None;
    }
    let expected_lines: Vec<&str> = expected.lines().collect();
    let actual_lines: Vec<&str> = actual.lines().collect();
    let mut report = format!("{name}: output differs from the golden file\n");
    for index in 0..expected_lines.len().max(actual_lines.len()) {
        let old = expected_lines.get(index).copied().unwrap_or("<missing>");
        let new = actual_lines.get(index).copied().unwrap_or("<missing>");
        if old != new {
            let _ = writeln!(report, "  old: {old}\n  new: {new}");
        }
    }
    Some(report)
}

/// Measures planar samples with the block meter in chunks of `chunk_frames`, failing the test on a meter error.
pub fn measure_signal(planes: &[Vec<f32>], sample_rate: u32, chunk_frames: usize) -> MeterOutput {
    let frames = planes.first().map_or(0, Vec::len);
    let mut meter =
        BlockMeter::new(planes.len() as u32, sample_rate, Some(frames as u64)).expect("meter accepts the stream");
    let mut offset = 0;
    while offset < frames {
        let end = (offset + chunk_frames.max(1)).min(frames);
        let chunk: Vec<&[f32]> = planes.iter().map(|plane| &plane[offset..end]).collect();
        meter.push(&chunk).expect("meter accepts the chunk");
        offset = end;
    }
    meter.finish(frames as u64).expect("meter finishes")
}

/// Fails the test with every collected difference.
pub fn assert_no_differences(differences: &[String]) {
    assert!(
        differences.is_empty(),
        "{} golden files differ, rerun with {UPDATE_VARIABLE}=1 only if the change is intended:\n{}",
        differences.len(),
        differences.join("\n")
    );
}
