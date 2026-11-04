//! Cross-language cache reuse against the C++ test helper and an explicit temporary database.
//! Set `AUDIOBATCH_CPP_TESTS` to the built `AudioBatchTests` executable to exercise both real cache implementations.

use std::env;
use std::io::{Read, Seek};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::thread;
use std::time::{Duration, Instant};

use audiobatch_core::discovery::normalize_path;
use audiobatch_core::record::MeasurementProducer;
use audiobatch_core::spectrogram::{FREQUENCY_ROWS, HOP_SIZE, SpectrogramData};
use audiobatch_core::{AnalysisCache, analyze_file};
use rusqlite::Connection;
use rusqlite::types::Value;

const HELPER_TIMEOUT: Duration = Duration::from_secs(30);
/// Size of the shared `SPEC` envelope that precedes the intensity grid.
const SPECTRAL_HEADER_BYTES: usize = 72;
/// Intensity cells per cell that may differ by one step between the two FFT libraries.
const FFT_DIFFERENCE_CELLS_PER_ALLOWED: usize = 1000;

/// Resolves the opt-in helper without requiring a C++ build in Rust-only CI jobs.
fn cpp_executable() -> Option<PathBuf> {
    let executable = env::var_os("AUDIOBATCH_CPP_TESTS").map(PathBuf::from);
    if executable.is_none() {
        eprintln!("Set AUDIOBATCH_CPP_TESTS to exercise the cross-language cache helper");
    }
    executable
}

/// Runs the C++ helper with bounded waits and file-backed output, avoiding pipe-capacity deadlocks.
fn cpp_output(executable: &Path, mode: &str, database: &Path, source: &Path) -> Vec<u8> {
    let mut stdout = tempfile::tempfile().expect("temporary helper stdout");
    let mut stderr = tempfile::tempfile().expect("temporary helper stderr");
    let mut child = Command::new(executable)
        .arg("--shared-cache")
        .arg(mode)
        .arg(database)
        .arg(source)
        .stdout(Stdio::from(stdout.try_clone().expect("stdout handle")))
        .stderr(Stdio::from(stderr.try_clone().expect("stderr handle")))
        .spawn()
        .expect("runs the C++ test helper");
    let started = Instant::now();
    let status = loop {
        if let Some(status) = child.try_wait().expect("polls C++ helper") {
            break status;
        }
        if started.elapsed() >= HELPER_TIMEOUT {
            child.kill().expect("stops timed-out C++ helper");
            child.wait().expect("reaps C++ helper");
            assert!(
                started.elapsed() < HELPER_TIMEOUT,
                "C++ helper {mode} exceeded {HELPER_TIMEOUT:?}"
            );
        }
        thread::sleep(Duration::from_millis(10));
    };
    stdout.rewind().expect("rewinds helper stdout");
    stderr.rewind().expect("rewinds helper stderr");
    let mut output = Vec::new();
    stdout.read_to_end(&mut output).expect("reads helper stdout");
    let mut errors = String::new();
    stderr.read_to_string(&mut errors).expect("reads helper stderr");
    assert!(status.success(), "C++ helper {mode}: {status}\n{errors}");
    output
}

/// Runs the C++ cache helper without touching either application's default database.
fn cpp_helper(executable: &Path, mode: &str, database: &Path, source: &Path) {
    cpp_output(executable, mode, database, source);
}

/// Captures an entire persisted row so reads and optional writes cannot silently mutate unrelated columns.
fn persisted_row(database: &Path, table: &str, source: &Path) -> Vec<Value> {
    let connection = Connection::open(database).expect("opens isolated database");
    let mut statement = connection
        .prepare(&format!("SELECT * FROM {table} WHERE file_path = ?1"))
        .expect("row snapshot statement");
    let columns = statement.column_count();
    statement
        .query_row([source.to_string_lossy().as_ref()], |row| {
            (0..columns).map(|column| row.get(column)).collect()
        })
        .expect("persisted row snapshot")
}

/// Reads the actual stored codec bytes rather than regenerating the source through another analyzer.
fn persisted_spectrum(database: &Path, source: &Path) -> Vec<u8> {
    Connection::open(database)
        .expect("opens isolated database")
        .query_row(
            "SELECT spectrogram_data FROM spectrogram_cache WHERE file_path = ?1",
            [source.to_string_lossy().as_ref()],
            |row| row.get(0),
        )
        .expect("persisted spectral bytes")
}

/// Confirms that the artifact writer is recorded independently of measurement provenance.
fn assert_spectrum_producer(database: &Path, source: &Path, expected: &str) {
    let producer: String = Connection::open(database)
        .expect("opens isolated database")
        .query_row(
            "SELECT producer FROM spectrogram_cache WHERE file_path = ?1",
            [source.to_string_lossy().as_ref()],
            |row| row.get(0),
        )
        .expect("artifact producer");
    assert_eq!(producer, expected);
}

/// Decodes the helper's text-safe export of C++ deserialization followed by serialization.
fn cpp_spectrum(executable: &Path, mode: &str, database: &Path, source: &Path) -> Vec<u8> {
    let output = cpp_output(executable, mode, database, source);
    let text = std::str::from_utf8(&output).expect("hexadecimal helper output").trim();
    assert_eq!(text.len() % 2, 0, "complete hexadecimal bytes");
    text.as_bytes()
        .as_chunks::<2>()
        .0
        .iter()
        .map(|pair| {
            u8::from_str_radix(std::str::from_utf8(pair).expect("hexadecimal pair"), 16)
                .expect("valid hexadecimal byte")
        })
        .collect()
}

/// Creates a source-matching grid covering every byte value independently of either FFT implementation.
fn patterned_spectrum(data: &SpectrogramData) -> SpectrogramData {
    SpectrogramData::new(
        data.sample_rate(),
        data.channels(),
        data.length_in_frames(),
        data.samples_per_column(),
        (0..data.intensities().len()).map(|index| (index % 256) as u8).collect(),
    )
    .expect("source-matching pattern geometry")
}

/// Checks independent `NumPy` peak powers, not low-level FFT leakage close to the fixed noise floor.
/// The one-byte tonal bound is the same contract as the per-language spectrogram fixture tests.
fn check_antiphase_calibration(data: &SpectrogramData, expected: &serde_json::Value) {
    assert!(data.matches_stream(48_000, 9600, 2));
    assert_eq!(data.column_count(), 10);
    assert_eq!(data.samples_per_column(), HOP_SIZE as u64);
    let source = expected.get("source").expect("oracle source");
    let bins = source
        .get("tone_bins")
        .and_then(serde_json::Value::as_array)
        .expect("tone bins");
    let probes = expected
        .get("probe_bins")
        .and_then(serde_json::Value::as_array)
        .expect("probe bins");
    let frames = expected
        .get("frames")
        .and_then(serde_json::Value::as_array)
        .expect("oracle frames");
    let interior = frames
        .iter()
        .find(|frame| frame["center_sample"].as_u64() == Some(5120))
        .expect("interior frame");
    for bin in bins {
        let bin = bin.as_u64().expect("tone bin");
        let probe = probes
            .iter()
            .position(|value| value.as_u64() == Some(bin))
            .expect("tone probe");
        let power = interior["powers"]
            .get(probe)
            .and_then(serde_json::Value::as_f64)
            .expect("independent power");
        let decibels = (10.0 * power.max(1.0e-12).log10()).clamp(-120.0, 24.0);
        let expected_byte = ((decibels + 120.0) * 255.0 / 144.0 + 0.5).floor() as u8;
        let frequency = bin as f64 * 48_000.0 / 4096.0;
        let row =
            (FREQUENCY_ROWS as f64 * (frequency / 700.0).ln_1p() / (24_000.0_f64 / 700.0).ln_1p()).floor() as usize;
        let actual = data
            .intensities()
            .get(5 * FREQUENCY_ROWS + row)
            .expect("calibrated row");
        assert!(
            actual.abs_diff(expected_byte) <= 1,
            "peak bin {bin}: {actual}, expected {expected_byte}"
        );
    }
}

/// Both production writers produce cache-only foreign hits without changing measurements, gains, or artifact provenance.
#[test]
fn both_implementations_reuse_production_spectra_without_reanalysis() {
    let Some(executable) = cpp_executable() else {
        return;
    };
    let directory = tempfile::tempdir().expect("temporary directory");
    let database = directory.path().join("spectra.db");
    let source = normalize_path(&test_fixtures::fixture("spectrogram_expected/antiphase_48000.wav"));
    cpp_helper(&executable, "write-spectrum", &database, &source);
    let cache = AnalysisCache::with_path(&database);
    cache.open().expect("Rust opens C++ spectral cache");
    let measurement = persisted_row(&database, "file_analysis", &source);
    let artifact = persisted_row(&database, "spectrogram_cache", &source);
    let cpp_bytes = persisted_spectrum(&database, &source);
    assert_spectrum_producer(&database, &source, "cpp");
    let foreign = cache.get_analysis(&source).expect("foreign measurements");
    assert_eq!(foreign.measurement_producer, MeasurementProducer::Cpp);
    assert!(
        foreign.spectrogram.is_none(),
        "measurement read does not hydrate spectra"
    );
    let loaded = cache.get_spectrogram(&source).expect("C++ spectrum reused in Rust");
    assert_eq!(
        loaded.to_bytes(),
        cpp_bytes,
        "C++ encoded bytes round-trip exactly in Rust"
    );
    assert_eq!(persisted_row(&database, "file_analysis", &source), measurement);
    assert_eq!(persisted_row(&database, "spectrogram_cache", &source), artifact);

    let oracle: serde_json::Value = serde_json::from_slice(
        &std::fs::read(test_fixtures::fixture("spectrogram_expected/expected.json")).expect("shared NumPy oracle"),
    )
    .expect("valid oracle");
    let expected = &oracle["files"]["antiphase_48000.wav"];
    check_antiphase_calibration(&loaded, expected);
    let native = analyze_file(&source, &|| false, &mut |_| {});
    assert!(native.is_ready(), "{}", native.error_message);
    let native_data = native.spectrogram.as_deref().expect("production Rust spectrum");
    check_antiphase_calibration(native_data, expected);
    assert!(
        cache
            .store_spectrogram(&foreign, native_data)
            .expect("Rust writes optional spectrum")
    );
    let rust_artifact = persisted_row(&database, "spectrogram_cache", &source);
    assert_spectrum_producer(&database, &source, "rust");
    assert_eq!(persisted_row(&database, "file_analysis", &source), measurement);
    assert_eq!(
        cpp_spectrum(&executable, "read-spectrum", &database, &source),
        native_data.to_bytes(),
        "Rust encoded bytes round-trip exactly in C++ without reanalysis",
    );
    assert_eq!(persisted_row(&database, "file_analysis", &source), measurement);
    assert_eq!(persisted_row(&database, "spectrogram_cache", &source), rust_artifact);
    assert_eq!(
        cache
            .get_analysis(&source)
            .expect("unchanged measurement producer")
            .measurement_producer,
        MeasurementProducer::Cpp,
    );
}

/// Both production analyzers produce matching spectral bytes for the same lossless PCM.
///
/// The headers must be identical. The FFT libraries differ, `juce::dsp::FFT` against `realfft`,
/// and their last-bit rounding can move a power across a quantization boundary.
/// That documented exception allows only one intensity step, in at most one cell per thousand and file.
/// Window, power normalization, mel support, quantization, and reduction are otherwise identical,
/// and the C++ sources disable floating-point contraction so compiler FMA choices cannot widen this bound.
#[test]
fn both_implementations_produce_matching_spectra_from_the_same_pcm() {
    let Some(executable) = cpp_executable() else {
        return;
    };
    let directory = tempfile::tempdir().expect("temporary directory");
    let unused_database = directory.path().join("unused.db");
    let fixtures = std::fs::read_dir(test_fixtures::fixture("spectrogram_expected")).expect("spectral fixtures");
    let mut names: Vec<String> = fixtures
        .map(|entry| entry.expect("fixture entry").path())
        .filter(|path| {
            path.extension()
                .is_some_and(|extension| extension.eq_ignore_ascii_case("wav"))
        })
        .filter_map(|path| path.file_name().map(|name| name.to_string_lossy().into_owned()))
        .collect();
    names.sort();
    assert!(!names.is_empty(), "spectral fixtures are present");
    let mut mismatches = Vec::new();
    for name in &names {
        let source = normalize_path(&test_fixtures::fixture(&format!("spectrogram_expected/{name}")));
        let native = analyze_file(&source, &|| false, &mut |_| {});
        assert!(native.is_ready(), "{name}: {}", native.error_message);
        let rust_bytes = native
            .spectrogram
            .as_deref()
            .expect("production Rust spectrum")
            .to_bytes();
        let cpp_bytes = cpp_spectrum(&executable, "analyze-spectrum", &unused_database, &source);
        assert_eq!(rust_bytes.len(), cpp_bytes.len(), "{name}: spectral geometry");
        let (rust_header, rust_grid) = rust_bytes.split_at(SPECTRAL_HEADER_BYTES);
        let (cpp_header, cpp_grid) = cpp_bytes.split_at(SPECTRAL_HEADER_BYTES);
        assert_eq!(rust_header, cpp_header, "{name}: identical codec header and geometry");
        let differing: Vec<(usize, u8, u8)> = rust_grid
            .iter()
            .zip(cpp_grid)
            .enumerate()
            .filter(|(_, (rust, cpp))| rust != cpp)
            .map(|(index, (rust, cpp))| (index, *rust, *cpp))
            .collect();
        let largest = differing
            .iter()
            .map(|(_, rust, cpp)| rust.abs_diff(*cpp))
            .max()
            .unwrap_or_default();
        let allowed_cells = rust_grid.len().div_ceil(FFT_DIFFERENCE_CELLS_PER_ALLOWED);
        if largest > 1 || differing.len() > allowed_cells {
            mismatches.push(format!(
                "{name}: {} cells differ (allowed {allowed_cells}), largest difference {largest}, first at {:?}",
                differing.len(),
                differing.first()
            ));
        }
    }
    assert!(
        mismatches.is_empty(),
        "spectral bytes exceed the documented FFT bound:\n{}",
        mismatches.join("\n")
    );
}

/// Every payload byte and the complete codec envelope survive C++ to Rust and Rust to C++ unchanged.
#[test]
fn deterministic_spectral_codec_is_byte_exact_in_both_directions() {
    let Some(executable) = cpp_executable() else {
        return;
    };
    let directory = tempfile::tempdir().expect("temporary directory");
    // Exercise C++-first and Rust-first schema creation, mono/stereo, and multiple sample rates.
    for (index, fixture) in ["antiphase_48000.wav", "tone_44100.wav", "silence_96000.wav"]
        .iter()
        .enumerate()
    {
        let database = directory.path().join(format!("pattern-{index}.db"));
        let source = normalize_path(&test_fixtures::fixture(&format!("spectrogram_expected/{fixture}")));
        let cache = AnalysisCache::with_path(&database);
        if index != 0 {
            cache.open().expect("Rust creates spectral schema first");
        }
        cpp_helper(&executable, "write-spectrum-pattern", &database, &source);
        cache.open().expect("opens shared spectral schema");
        let record = cache.get_analysis(&source).expect("shared source measurements");
        let measurement = persisted_row(&database, "file_analysis", &source);
        let cpp_artifact = persisted_row(&database, "spectrogram_cache", &source);
        assert_spectrum_producer(&database, &source, "cpp");
        let data = cache.get_spectrogram(&source).expect("C++ pattern read by Rust");
        let pattern = patterned_spectrum(&data);
        assert_eq!(*data, pattern, "C++ pattern preserves all byte values and geometry");
        assert_eq!(persisted_spectrum(&database, &source), pattern.to_bytes());
        assert_eq!(persisted_row(&database, "spectrogram_cache", &source), cpp_artifact);
        cache
            .store_custom_gain_for_record(&record, Some(-2.0))
            .expect("explicit gain");
        let measurement_with_gain = persisted_row(&database, "file_analysis", &source);
        assert!(cache.store_spectrogram(&record, &pattern).expect("Rust pattern write"));
        let rust_artifact = persisted_row(&database, "spectrogram_cache", &source);
        assert_spectrum_producer(&database, &source, "rust");
        assert_eq!(
            cpp_spectrum(&executable, "read-spectrum-pattern", &database, &source),
            pattern.to_bytes(),
        );
        assert_eq!(
            persisted_row(&database, "file_analysis", &source),
            measurement_with_gain
        );
        assert_eq!(persisted_row(&database, "spectrogram_cache", &source), rust_artifact);
        assert_eq!(record.measurement_producer, MeasurementProducer::Cpp);
        assert_ne!(measurement, measurement_with_gain, "gain edit is exercised");
    }
}

/// Verifies both real cache implementations reuse each other's measurements while preserving independent artifacts.
#[test]
fn both_implementations_reuse_the_other_producers_measurements() {
    let Some(executable) = cpp_executable() else {
        return;
    };
    let directory = tempfile::tempdir().expect("temporary directory");
    let database = directory.path().join("analysis.db");
    let cache = AnalysisCache::with_path(&database);
    // Include lossy reporting reuse, silence, partial loudness windows, and both creation orders.
    for fixture in [
        "sine_stereo_44100_16.wav",
        "sine_stereo_44100_320k.mp3",
        "silence_stereo_44100_16.wav",
        "sine_mono_24000_16.wav",
    ] {
        let source = normalize_path(&test_fixtures::fixture(fixture));
        cpp_helper(&executable, "write", &database, &source);
        cache.open().expect("opens the shared database");
        let foreign = cache.get_analysis(&source).expect("C++ measurement hit in Rust");
        assert!(foreign.from_cache && foreign.is_ready());
        assert_eq!(foreign.measurement_producer, MeasurementProducer::Cpp);
        assert!(
            foreign.waveform.is_none(),
            "measurement reads must not hydrate waveforms"
        );
        let native = analyze_file(&source, &|| false, &mut |_| {});
        assert!(native.is_ready(), "{}", native.error_message);
        cache.store_analysis(&native).expect("writes Rust measurements");
        assert!(
            cache.get_waveform(&source).is_some(),
            "native Rust waveform persisted separately"
        );
        cpp_helper(&executable, "read-rust", &database, &source);
        cache
            .store_custom_gain_for_record(&native, Some(-2.0))
            .expect("explicit Rust gain edit");
        cpp_helper(&executable, "write", &database, &source);
        let after_cpp = cache.get_analysis(&source).expect("C++ refill preserves gain");
        assert!(
            after_cpp
                .custom_gain_db
                .is_some_and(|gain| (gain + 2.0).abs() < f32::EPSILON)
        );
        assert!(
            cache.get_waveform(&source).is_some(),
            "C++ refill preserves compatible Rust waveform"
        );
        cache
            .store_custom_gain_for_record(&after_cpp, None)
            .expect("explicit clear");
        cache
            .store_analysis(&native)
            .expect("stale in-memory gain cannot override clear");
        assert!(
            cache
                .get_analysis(&source)
                .expect("cleared row")
                .custom_gain_db
                .is_none()
        );
    }
    let rust_first_database = directory.path().join("rust-first.db");
    let rust_first = AnalysisCache::with_path(&rust_first_database);
    rust_first.open().expect("Rust creates the database");
    let source = normalize_path(&test_fixtures::fixture("sine_stereo_44100_16.wav"));
    rust_first
        .store_analysis(&analyze_file(&source, &|| false, &mut |_| {}))
        .expect("Rust writes first");
    cpp_helper(&executable, "read-rust", &rust_first_database, &source);

    let simultaneous_database = directory.path().join("simultaneous.db");
    let mut cpp = Command::new(&executable)
        .arg("--shared-cache")
        .arg("write")
        .arg(&simultaneous_database)
        .arg(&source)
        .spawn()
        .expect("starts C++ while Rust opens the same fresh database");
    let simultaneous = AnalysisCache::with_path(&simultaneous_database);
    if simultaneous.open().is_err() {
        simultaneous.open().expect("startup retry");
    }
    assert!(cpp.wait().expect("C++ completes").success());
    assert_eq!(
        simultaneous
            .get_analysis(&source)
            .expect("shared startup success")
            .measurement_producer,
        MeasurementProducer::Cpp
    );
}
