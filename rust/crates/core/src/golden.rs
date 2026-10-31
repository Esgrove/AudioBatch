//! Regression snapshots of our loudness meter, peak measurements, and normalization output.
//!
//! The level tests elsewhere check values within decibel tolerances, which a change in the last bit passes.
//! These tests record every value with its exact bits in a small text file per input in `tests/golden/`,
//! as `f64::to_bits` hex next to the decimal value for reading,
//! and fail when any value differs, printing the old and the new line.
//! Windows x86-64 uses overrides in `tests/golden/windows-x86_64/` only where platform math differs.
//! All other outputs use the shared snapshots, and input identities remain shared on every platform.
//! Running the tests with `UPDATE_GOLDEN=1` rewrites the files.
//! Only regenerate them in a commit that explains why the output changed, never inside an optimization.
//! The inputs are the committed fixtures, the Git LFS fixtures in `fixtures/normalization` when present,
//! and in-memory signals at every rate where the true peak oversampling changes.

use std::fmt::Write as _;
use std::fs;
use std::hash::Hasher as _;
use std::io::ErrorKind;
use std::path::{Path, PathBuf};
use std::process::Command;

use crate::cache::serialize_history;
use crate::meter::{BlockMeter, MeterOutput};
use crate::record::AnalysisRecord;
use crate::signals::SignalKind;

/// Environment variable that rewrites the golden files instead of comparing against them.
const UPDATE_VARIABLE: &str = "UPDATE_GOLDEN";

/// Only targets with independently validated numerical baselines may select overrides.
const PLATFORM_DIRECTORY: Option<&str> = if cfg!(all(target_os = "windows", target_arch = "x86_64")) {
    Some("windows-x86_64")
} else {
    None
};

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
        let directory = Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/golden");
        if std::env::var_os(UPDATE_VARIABLE).is_some() {
            write_golden(&directory, name, PLATFORM_DIRECTORY, &self.text);
            return None;
        }
        let path = golden_path(&directory, name, PLATFORM_DIRECTORY);
        let Some(expected) = read_golden(&path) else {
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

/// Selects an existing platform override, otherwise the shared snapshot.
fn golden_path(directory: &Path, name: &str, platform: Option<&str>) -> PathBuf {
    if let Some(platform) = platform {
        let path = directory.join(platform).join(format!("{name}.txt"));
        if path.try_exists().expect("checks for a platform golden file") {
            return path;
        }
    }
    directory.join(format!("{name}.txt"))
}

/// Reads a snapshot, treating only a missing file as an absent baseline.
fn read_golden(path: &Path) -> Option<String> {
    match fs::read_to_string(path) {
        Ok(text) => Some(text),
        Err(error) if error.kind() == ErrorKind::NotFound => None,
        Err(error) => panic!("cannot read golden file {}: {error}", path.display()),
    }
}

/// Updates only the current platform, omitting or removing overrides identical to the shared snapshot.
fn write_golden(directory: &Path, name: &str, platform: Option<&str>, text: &str) {
    let shared = directory.join(format!("{name}.txt"));
    let path = if let Some(platform) = platform {
        let path = directory.join(platform).join(format!("{name}.txt"));
        if read_golden(&shared).is_some_and(|expected| expected == text) {
            match fs::remove_file(&path) {
                Ok(()) => {}
                Err(error) if error.kind() == ErrorKind::NotFound => {}
                Err(error) => panic!("cannot remove redundant golden file {}: {error}", path.display()),
            }
            return;
        }
        path
    } else {
        shared
    };
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent).expect("creates the golden directory");
    }
    fs::write(&path, text).expect("writes the golden file");
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
        meter.push(&chunk);
        offset = end;
    }
    meter.finish(frames as u64)
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

/// Writes an opt-in numerical report separately from all regression snapshots.
///
/// # Panics
///
/// Panics if compiler information cannot be read or the diagnostic directory cannot be written.
pub fn write_diagnostic_report(name: &str, text: &GoldenText) {
    let workspace = Path::new(env!("CARGO_MANIFEST_DIR")).join("../..");
    let directory = workspace.join(
        std::env::var_os("AUDIOBATCH_DIAGNOSTICS_DIR")
            .map_or_else(|| PathBuf::from("target/numerical-diagnostics"), PathBuf::from),
    );
    fs::create_dir_all(&directory).expect("creates the numerical diagnostic directory");
    let compiler = Command::new("rustc")
        .args(["--version", "--verbose"])
        .output()
        .expect("reads compiler version");
    assert!(compiler.status.success(), "compiler version command failed");
    let mut report = String::new();
    let _ = writeln!(report, "os = {}", std::env::consts::OS);
    let _ = writeln!(report, "architecture = {}", std::env::consts::ARCH);
    let _ = writeln!(report, "debug_assertions = {}", cfg!(debug_assertions));
    let _ = writeln!(report, "rustflags = {:?}", std::env::var_os("RUSTFLAGS"));
    let _ = writeln!(report, "compiled_avx2 = {}", cfg!(target_feature = "avx2"));
    let _ = writeln!(report, "compiled_fma = {}", cfg!(target_feature = "fma"));
    let _ = writeln!(report, "compiled_neon = {}", cfg!(target_feature = "neon"));
    #[cfg(target_arch = "x86_64")]
    let _ = writeln!(report, "runtime_avx2 = {}", std::arch::is_x86_feature_detected!("avx2"));
    report.push_str(&String::from_utf8(compiler.stdout).expect("compiler version is UTF-8"));
    report.push_str(text.text());
    fs::write(directory.join(format!("{name}.txt")), report).expect("writes numerical diagnostic report");
}

/// The two differing DC tails and the 192 kHz channel-count corpus, without interpolation.
pub fn diagnostic_signals() -> impl Iterator<Item = (SignalKind, u32, usize, f64)> {
    [(SignalKind::Dc, 88_200, 2, 3.1), (SignalKind::Dc, 192_000, 2, 3.1)]
        .into_iter()
        .chain(
            [1, 2, 3, 4, 6, 8]
                .into_iter()
                .map(|channels| (SignalKind::MusicLike, 192_000, channels, 0.45)),
        )
}

#[cfg(test)]
mod test_platform_goldens {
    use std::fs;

    use super::{difference, golden_path, read_golden, write_golden};

    #[test]
    fn missing_override_uses_shared_snapshot() {
        let directory = tempfile::tempdir().expect("creates the golden directory");
        write_golden(directory.path(), "signal/dc", None, "shared\n");
        let path = golden_path(directory.path(), "signal/dc", Some("windows-x86_64"));
        assert_eq!(path, directory.path().join("signal/dc.txt"));
        assert_eq!(read_golden(&path).as_deref(), Some("shared\n"));
    }

    #[test]
    fn override_is_selected_only_for_its_platform() {
        let directory = tempfile::tempdir().expect("creates the golden directory");
        write_golden(directory.path(), "signal/dc", None, "shared\n");
        write_golden(directory.path(), "signal/dc", Some("windows-x86_64"), "windows\n");
        let windows = golden_path(directory.path(), "signal/dc", Some("windows-x86_64"));
        let shared = golden_path(directory.path(), "signal/dc", None);
        assert_eq!(read_golden(&windows).as_deref(), Some("windows\n"));
        assert_eq!(read_golden(&shared).as_deref(), Some("shared\n"));
    }

    #[test]
    fn identical_outputs_do_not_create_overrides() {
        let directory = tempfile::tempdir().expect("creates the golden directory");
        write_golden(directory.path(), "signal/dc", None, "shared\n");
        write_golden(directory.path(), "signal/dc", Some("windows-x86_64"), "shared\n");
        assert!(!directory.path().join("windows-x86_64").exists());
    }

    #[test]
    fn converged_outputs_remove_only_the_override() {
        let directory = tempfile::tempdir().expect("creates the golden directory");
        write_golden(directory.path(), "signal/dc", None, "shared\n");
        write_golden(directory.path(), "signal/dc", Some("windows-x86_64"), "windows\n");
        write_golden(directory.path(), "signal/dc", Some("windows-x86_64"), "shared\n");
        assert!(!directory.path().join("windows-x86_64/signal/dc.txt").exists());
        assert_eq!(
            fs::read_to_string(directory.path().join("signal/dc.txt")).expect("reads the shared snapshot"),
            "shared\n"
        );
    }

    #[test]
    fn last_bit_differences_still_fail() {
        assert!(
            difference(
                "signal/dc",
                "level = 1.0 [3ff0000000000000]\n",
                "level = 1.0000000000000002 [3ff0000000000001]\n",
            )
            .is_some()
        );
    }
}
