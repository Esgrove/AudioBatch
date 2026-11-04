//! Shared `NumPy` expectations, streaming invariance, cache persistence, and quantized-output readback.

use audiobatch_core::meter::BlockMeter;
use audiobatch_core::signal_metrics::SignalMetrics;
use audiobatch_core::{AnalysisCache, analyze_file};
use serde_json::Value;

/// Loads the same independently generated expectations consumed by the C++ tests.
fn expectations() -> Value {
    serde_json::from_str(include_str!("../../../../fixtures/metrics_expected.json")).expect("shared expectations")
}

/// Compares every new statistic with the independent batch `NumPy` calculation.
fn assert_expected(metrics: &SignalMetrics, expected: &Value) {
    for (actual, name) in [
        (metrics.crest_factor_db, "crestFactorDb"),
        (metrics.stereo_correlation, "stereoCorrelation"),
    ] {
        match (actual, expected[name].as_f64()) {
            (Some(actual), Some(expected)) => {
                assert!((actual - expected).abs() < 1e-10, "{name}: {actual} != {expected}");
            }
            (None, None) => {}
            (actual, expected) => assert_eq!(actual, expected, "{name} availability"),
        }
    }
    let offsets = expected["dcOffsets"].as_array().expect("means");
    assert_eq!(metrics.dc_offsets.len(), offsets.len());
    for (actual, expected) in metrics.dc_offsets.iter().zip(offsets) {
        assert!((actual - expected.as_f64().expect("mean")).abs() < 1e-12);
    }
    let counts: Vec<_> = expected["clipCounts"]
        .as_array()
        .expect("counts")
        .iter()
        .map(|value| value.as_u64().expect("count"))
        .collect();
    assert_eq!(metrics.clip_counts, counts);
    assert_eq!(metrics.total_clips(), counts.iter().sum::<u64>());
}

/// Measures exact float32 planes in deliberately unrelated packet sizes.
fn measure(planes: &[Vec<f32>], chunk: usize) -> SignalMetrics {
    let frames = planes.first().expect("nonempty planes").len();
    let mut meter = BlockMeter::new(planes.len() as u32, 44_100, Some(frames as u64)).expect("meter");
    for start in (0..frames).step_by(chunk) {
        let end = (start + chunk).min(frames);
        let slices: Vec<_> = planes
            .iter()
            .map(|plane| plane.get(start..end).expect("bounded chunk"))
            .collect();
        meter.push(&slices);
    }
    meter.finish(frames as u64).signal_metrics
}

#[test]
fn synthetic_statistics_and_chunk_invariance_match_numpy() {
    for signal in expectations()["signals"].as_array().expect("signals") {
        let planes: Vec<Vec<f32>> = signal["planes"]
            .as_array()
            .expect("planes")
            .iter()
            .map(|plane| {
                plane
                    .as_array()
                    .expect("samples")
                    .iter()
                    .map(|sample| sample.as_f64().expect("sample") as f32)
                    .collect()
            })
            .collect();
        let reference = measure(&planes, planes.first().expect("nonempty planes").len());
        assert_expected(&reference, signal);
        for chunk in [1, 3, 7, 64] {
            assert_eq!(measure(&planes, chunk), reference);
        }
        // Repeat enough PCM to cross fixed meter intervals without changing the sample distribution.
        let long: Vec<_> = planes.iter().map(|plane| plane.repeat(1500)).collect();
        let reference = measure(&long, 12_000);
        for chunk in [1, 7, 4096, 4410, 4411] {
            assert_eq!(measure(&long, chunk), reference);
        }
    }
}

#[test]
fn fixture_statistics_and_required_cache_payloads_match_numpy() {
    for fixture in expectations()["fixtures"].as_array().expect("fixtures") {
        let path = test_fixtures::fixture(fixture["fixture"].as_str().expect("name"));
        let record = analyze_file(&path, &|| false, &mut |_| {});
        assert!(record.is_ready());
        assert_expected(&record.signal_metrics, fixture);
        for (actual, key) in [
            (record.signal_metrics.plr_db, "plrDb"),
            (record.signal_metrics.psr_db, "psrDb"),
        ] {
            match (actual, fixture[key].as_f64()) {
                (Some(actual), Some(expected)) => {
                    assert!((actual - expected).abs() < 0.001, "{key}: {actual} != {expected}");
                }
                (actual, expected) => assert_eq!(actual, expected, "{key} availability"),
            }
        }
        for (ratio, loudness) in [
            (record.signal_metrics.plr_db, record.integrated_lufs),
            (record.signal_metrics.psr_db, record.max_short_term_lufs),
        ] {
            if let Some(ratio) = ratio {
                assert!((ratio - (20.0_f64.mul_add(record.overall_true_peak.log10(), -loudness))).abs() < 1e-12);
            } else {
                assert!(record.overall_true_peak <= 0.0 || loudness <= -1000.0);
            }
        }
        let cache = AnalysisCache::in_memory();
        cache.store_analysis(&record).expect("store metrics");
        assert_eq!(
            cache.get_analysis(&path).expect("cache hit").signal_metrics,
            record.signal_metrics
        );
    }
}

#[test]
fn normalized_metrics_describe_quantized_output_not_source_or_preview_gain() {
    let directory = tempfile::tempdir().expect("scratch directory");
    let path = directory.path().join("tone.wav");
    std::fs::copy(test_fixtures::fixture("stereo_split_44100_16.wav"), &path).expect("fixture copy");
    let mut source = analyze_file(&path, &|| false, &mut |_| {});
    source.custom_gain_db = Some(3.0);
    let original = source.signal_metrics.clone();
    let options = audiobatch_core::normalize::NormalizationOptions {
        dispose: std::sync::Arc::new(|path| std::fs::remove_file(path).is_ok()),
        ..Default::default()
    };
    let result = audiobatch_core::normalize::normalize_file(&source, &options, &|| false, &mut |_| {});
    let output = result.output().expect("normalized output");
    let readback = analyze_file(&output.path, &|| false, &mut |_| {});
    assert_eq!(output.signal_metrics, readback.signal_metrics);
    assert_eq!(source.signal_metrics, original);
}
