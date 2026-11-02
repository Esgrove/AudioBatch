//! Cross-language cache reuse against the C++ test helper and an explicit temporary database.
//! Set `AUDIOBATCH_CPP_TESTS` to the built `AudioBatchTests` executable to exercise both real cache implementations.

use std::env;
use std::path::{Path, PathBuf};
use std::process::Command;

use audiobatch_core::discovery::normalize_path;
use audiobatch_core::record::MeasurementProducer;
use audiobatch_core::{AnalysisCache, analyze_file};

/// Runs the C++ cache helper without touching either application's default database.
fn cpp_helper(executable: &Path, mode: &str, database: &Path, source: &Path) {
    let status = Command::new(executable)
        .arg("--shared-cache")
        .arg(mode)
        .arg(database)
        .arg(source)
        .status()
        .expect("runs the C++ test helper");
    assert!(status.success(), "C++ helper {mode}: {status}");
}

#[test]
fn both_implementations_reuse_the_other_producers_measurements() {
    let Some(executable) = env::var_os("AUDIOBATCH_CPP_TESTS").map(PathBuf::from) else {
        eprintln!("Set AUDIOBATCH_CPP_TESTS to exercise the cross-language cache helper");
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
