//! Benchmarks of the work around the analysis: reading cached records, and sorting and comparing names.
//!
//! A cached run of a large folder loads every record from SQLite, including its history and waveform blobs,
//! and the CLI and the GUI sort the records by name, path, peak, or loudness.

// Benchmarks are test code, which the workspace allows to index, like the tests.
#![allow(clippy::indexing_slicing)]

use std::fs;
use std::hint::black_box;
use std::path::PathBuf;

use audiobatch_core::record::file_identity;
use audiobatch_core::sorting::{compare_natural, sort_records};
use audiobatch_core::{AnalysisCache, AnalysisRecord, SortMode, analyze_file};
use criterion::{BatchSize, BenchmarkId, Criterion, Throughput, criterion_group, criterion_main};

/// Number of cached rows read back.
const CACHED_ROWS: usize = 500;

/// Number of records sorted.
const SORTED_RECORDS: usize = 5_000;

/// Returns a varied file name, with numbers so the natural order differs from the byte order.
fn file_name(index: usize) -> String {
    let artists = ["Alpha", "beta", "Gamma", "delta", "Äänekoski"];
    format!(
        "{} - Track {} (Mix {}).flac",
        artists[index % artists.len()],
        index % 97,
        index % 13
    )
}

/// Benchmarks `get_analysis` over a cache of real records, one file per row.
fn bench_cache(criterion: &mut Criterion) {
    let directory = tempfile::tempdir().expect("creates directory");
    let fixture = test_fixtures::fixture("sine_stereo_44100_16.wav");
    let template = analyze_file(&fixture, &|| false, &mut |_| {});
    let cache = AnalysisCache::with_path(directory.path().join("cache.db"));
    cache.open().expect("opens the cache");
    let mut paths: Vec<PathBuf> = Vec::with_capacity(CACHED_ROWS);
    for index in 0..CACHED_ROWS {
        let path = directory.path().join(format!("{index}.wav"));
        fs::copy(&fixture, &path).expect("copies the fixture");
        let mut record = template.clone();
        record.path.clone_from(&path);
        (record.file_size, record.modified_time_ms) = file_identity(&path).expect("the copy exists");
        cache.store_analysis(&record).expect("stores the record");
        paths.push(path);
    }

    let mut group = criterion.benchmark_group("cache");
    group.throughput(Throughput::Elements(CACHED_ROWS as u64));
    group.bench_function("get_analysis", |bencher| {
        bencher.iter(|| {
            paths
                .iter()
                .filter(|path| cache.get_analysis(black_box(path)).is_some())
                .count()
        });
    });
    group.finish();
}

/// Benchmarks natural name comparison and sorting records in every mode.
fn bench_sorting(criterion: &mut Criterion) {
    let template = AnalysisRecord {
        waveform: None,
        short_term_loudness: None,
        ..analyze_file(
            &test_fixtures::fixture("sine_stereo_44100_16.wav"),
            &|| false,
            &mut |_| {},
        )
    };
    let records: Vec<AnalysisRecord> = (0..SORTED_RECORDS)
        .map(|index| {
            let name = file_name(index);
            let mut record = template.clone();
            record.path = PathBuf::from(format!("/music/{}/{name}", index % 37));
            record.file_name = name;
            record.overall_peak = ((index * 7919) % 1000) as f32 / 1000.0;
            record.integrated_lufs = -(((index * 104_729) % 300) as f64) / 10.0;
            record
        })
        .collect();
    let names: Vec<String> = (0..SORTED_RECORDS).map(file_name).collect();

    let mut group = criterion.benchmark_group("sorting");
    group.throughput(Throughput::Elements(SORTED_RECORDS as u64));
    group.bench_function("compare_natural", |bencher| {
        bencher.iter(|| {
            names
                .windows(2)
                .filter(|pair| compare_natural(black_box(&pair[0]), black_box(&pair[1])).is_lt())
                .count()
        });
    });
    for (label, mode) in [
        ("name", SortMode::Name),
        ("path", SortMode::Path),
        ("peak", SortMode::Peak),
        ("lufs", SortMode::Loudness),
    ] {
        group.bench_function(BenchmarkId::new("sort_records", label), |bencher| {
            bencher.iter_batched(
                || records.clone(),
                |mut records| {
                    sort_records(&mut records, mode, true);
                    records
                },
                BatchSize::LargeInput,
            );
        });
    }
    group.finish();
}

criterion_group!(benches, bench_cache, bench_sorting);
criterion_main!(benches);
