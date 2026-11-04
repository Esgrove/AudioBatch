//! Benchmarks of the work around the analysis: reading cached records, and sorting and comparing names.
//!
//! A cached run loads measurements and bounded histories without retaining native preview blobs.
//! Selected spectral reads and independent spectral publication include identity checks and portable blob processing.
//! The CLI and the GUI sort the records by name, path, peak, or loudness.

// Benchmarks are test code, which the workspace allows to index, like the tests.
#![expect(
    clippy::indexing_slicing,
    reason = "benchmarks are test code, where indexing is allowed"
)]

use std::env;
use std::fs;
use std::hint::black_box;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Barrier};
use std::thread;
use std::time::{Duration, Instant};

use audiobatch_core::overload::{OverloadHistoryBuilder, SAMPLE_OVER};
use audiobatch_core::record::file_identity;
use audiobatch_core::sorting::{compare_natural, sort_records};
use audiobatch_core::waveform::WaveformBuilder;
use audiobatch_core::{
    AnalysisCache, AnalysisRecord, MeasurementProducer, ShortTermLoudnessHistory, SortMode, analyze_file,
};
use criterion::{BatchSize, BenchmarkId, Criterion, Throughput, criterion_group, criterion_main};
use rusqlite::Connection;

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

/// Builds ten-minute measurement histories and a bounded preview independently of fixture duration.
fn representative_cache_record() -> AnalysisRecord {
    let fixture = test_fixtures::fixture("sine_stereo_44100_16.wav");
    let mut template = analyze_file(&fixture, &|| false, &mut |_| {});
    template.length_in_samples = 44_100 * 600;
    template.duration_seconds = 600.0;
    template.short_term_loudness = Some(Arc::new(ShortTermLoudnessHistory {
        values: vec![-14.0; 6000],
        sample_rate: 44_100.0,
        length_in_samples: template.length_in_samples,
        samples_per_point: 4410,
    }));
    let mut overloads = OverloadHistoryBuilder::new(2, 44_100);
    for _ in 0..6000 {
        overloads.push(&[0, SAMPLE_OVER]);
    }
    template.overloads = Some(Arc::new(overloads.finish(template.length_in_samples)));
    template.waveform = Some(representative_waveform(&template));
    template
}

/// Builds a simulated ten-minute stereo preview without retaining decoded audio or running a measurement meter.
fn representative_waveform(record: &AnalysisRecord) -> Arc<audiobatch_core::WaveformOverview> {
    let mut waveform = WaveformBuilder::new(record.channels as usize, Some(record.length_in_samples));
    let block = [0.5; 4096];
    let mut remaining = record.length_in_samples as usize;
    while remaining > 0 {
        let take = remaining.min(block.len());
        waveform.push(&[&block[..take], &block[..take]]);
        remaining -= take;
    }
    waveform.finish(record.sample_rate, record.length_in_samples)
}

/// Loads and retains a complete folder, checking that only measurements and their required histories are hydrated.
fn retained_measurements(cache: &AnalysisCache, records: &[AnalysisRecord]) -> Vec<AnalysisRecord> {
    let loaded: Vec<_> = records
        .iter()
        .map(|record| cache.get_analysis(black_box(&record.path)).expect("warm hit"))
        .collect();
    assert!(loaded.iter().all(|record| record.waveform.is_none()));
    black_box(loaded)
}

/// Reports retained history payloads and independently checks that the folder keeps none of its on-disk previews.
fn report_retained_folder(cache: &AnalysisCache, records: &[AnalysisRecord], template: &AnalysisRecord) {
    let waveform_bytes = template
        .waveform
        .as_ref()
        .expect("representative preview")
        .to_bytes()
        .len();
    let loaded = retained_measurements(cache, records);
    let history_bytes: usize = loaded
        .iter()
        .map(|record| {
            record
                .short_term_loudness
                .as_ref()
                .expect("loudness history")
                .values
                .len()
                * size_of::<f32>()
                + record.overloads.as_ref().expect("overload history").flags.len()
        })
        .sum();
    eprintln!(
        "Retained folder: {} rows, {history_bytes} history payload bytes, zero waveforms, {} serialized preview bytes excluded",
        loaded.len(),
        waveform_bytes * loaded.len(),
    );
}

/// Reports empirical latency percentiles, without treating them as a statistical confidence interval.
fn report_latencies(label: &str, latencies: &mut [Duration]) {
    latencies.sort_unstable();
    let count = latencies.len();
    assert!(count > 0, "latency probes must run");
    eprintln!(
        "{label}: {count} probes, p50 {:.3} ms, p95 {:.3} ms, maximum {:.3} ms",
        latencies[count / 2].as_secs_f64() * 1000.0,
        latencies[(count * 95).div_ceil(100) - 1].as_secs_f64() * 1000.0,
        latencies[count - 1].as_secs_f64() * 1000.0,
    );
}

/// Times independent periodic foreground work, first idle and then alongside one synthetic waveform-filling worker.
/// Cache probes intentionally share the connection mutex, unlike GUI gain edits, which run on a separate worker.
fn measure_waveform_contention(cache: &Arc<AnalysisCache>, database: &Path, records: &[AnalysisRecord]) {
    const PREVIEW_ROWS: usize = 16;
    const IDLE_PROBES: usize = 64;
    const PROBE_INTERVAL: Duration = Duration::from_millis(2);

    let connection = Connection::open(database).expect("opens preview reset connection");
    for record in records.iter().take(PREVIEW_ROWS) {
        connection
            .execute(
                "DELETE FROM rust_waveform_cache WHERE file_path = ?1",
                [record.path.to_string_lossy()],
            )
            .expect("clears native preview for background repair");
        assert!(cache.get_waveform(&record.path).is_none());
    }
    drop(connection);
    for filling in [false, true] {
        let started = Arc::new(Barrier::new(2));
        let finished = Arc::new(AtomicBool::new(false));
        let worker = filling.then(|| {
            let cache = Arc::clone(cache);
            let started = Arc::clone(&started);
            let finished = Arc::clone(&finished);
            let records: Vec<_> = records.iter().take(PREVIEW_ROWS).cloned().collect();
            thread::spawn(move || {
                started.wait();
                let began = Instant::now();
                for record in &records {
                    let waveform = representative_waveform(record);
                    let weak = Arc::downgrade(&waveform);
                    assert!(cache.store_waveform(record, &waveform).expect("fills native preview"));
                    drop(waveform);
                    assert!(
                        weak.upgrade().is_none(),
                        "unselected preview must be released after persistence"
                    );
                }
                finished.store(true, Ordering::Release);
                eprintln!(
                    "Background waveform fill: {} simulated ten-minute previews in {:.3} ms, none retained",
                    records.len(),
                    began.elapsed().as_secs_f64() * 1000.0,
                );
            })
        });
        if filling {
            started.wait();
        }
        let mut wake_delays = Vec::new();
        let mut read_latencies = Vec::new();
        let mut gain_latencies = Vec::new();
        let mut next_probe = Instant::now() + PROBE_INTERVAL;
        loop {
            thread::sleep(next_probe.saturating_duration_since(Instant::now()));
            wake_delays.push(Instant::now().saturating_duration_since(next_probe));
            let record = &records[read_latencies.len() % records.len()];
            let began = Instant::now();
            let loaded = cache.get_analysis(&record.path).expect("foreground measurement hit");
            read_latencies.push(began.elapsed());
            assert!(loaded.waveform.is_none());
            let began = Instant::now();
            assert!(
                cache
                    .store_custom_gain_for_record(record, Some(-3.0))
                    .expect("foreground gain probe")
            );
            gain_latencies.push(began.elapsed());
            // Schedule from completion, so slow probes do not create a catch-up burst.
            next_probe = Instant::now() + PROBE_INTERVAL;
            if (filling && finished.load(Ordering::Acquire)) || (!filling && read_latencies.len() == IDLE_PROBES) {
                break;
            }
        }
        if let Some(worker) = worker {
            worker.join().expect("waveform filler exits");
        }
        let label = if filling {
            "During waveform fill"
        } else {
            "Idle baseline"
        };
        report_latencies(&format!("{label} synthetic wake delay"), &mut wake_delays);
        report_latencies(&format!("{label} measurement read"), &mut read_latencies);
        report_latencies(&format!("{label} gain edit"), &mut gain_latencies);
    }
}

/// Benchmarks native and foreign warm reads with long histories, separate previews, and a concurrent gain writer.
/// Retaining a folder's measurement rows verifies that previews are never hydrated en masse.
fn bench_shared_cache(criterion: &mut Criterion) {
    let directory = tempfile::tempdir().expect("creates directory");
    let fixture = test_fixtures::fixture("sine_stereo_44100_16.wav");
    let template = representative_cache_record();
    let mut records = Vec::with_capacity(CACHED_ROWS);
    let database = directory.path().join("shared.db");
    let cache = Arc::new(AnalysisCache::with_path(&database));
    for index in 0..CACHED_ROWS {
        let mut record = template.clone();
        record.path = directory.path().join(format!("{index}.wav"));
        fs::copy(&fixture, &record.path).expect("copies fixture");
        (record.file_size, record.modified_time_ms) = file_identity(&record.path).expect("copy identity");
        cache.store_analysis(&record).expect("stores representative record");
        record.waveform = None;
        records.push(record);
    }
    if let Some(executable) = env::var_os("AUDIOBATCH_CPP_TESTS") {
        let status = Command::new(executable)
            .args(["--shared-cache", "benchmark-read"])
            .arg(&database)
            .arg(directory.path())
            .status()
            .expect("runs native C++ warm reads of Rust measurements");
        assert!(status.success(), "C++ must accept every Rust warm row");
    }
    report_retained_folder(&cache, &records, &template);
    measure_waveform_contention(&cache, &database, &records);
    let wal_path = directory.path().join("shared.db-wal");
    let mut group = criterion.benchmark_group("cache/shared");
    for producer in [MeasurementProducer::Rust, MeasurementProducer::Cpp] {
        for record in &mut records {
            record.measurement_producer = producer;
            record.waveform = None;
            cache
                .store_analysis(record)
                .expect("sets producer without replacing previews");
        }
        group.throughput(Throughput::Elements(1));
        group.bench_function(format!("{producer:?}/first_result_new_connection"), |bencher| {
            bencher.iter_batched(
                || AnalysisCache::with_path(&database),
                |reader| {
                    let record = reader.get_analysis(&records[0].path).expect("first measurement hit");
                    assert!(record.waveform.is_none());
                    // Return the reader so its connection teardown is outside the measured operation.
                    (reader, record)
                },
                BatchSize::PerIteration,
            );
        });
        group.throughput(Throughput::Elements(CACHED_ROWS as u64));
        group.bench_function(
            format!("{producer:?}/retained_measurements_without_waveforms"),
            |bencher| bencher.iter(|| retained_measurements(&cache, &records)),
        );
    }
    let wal_before = fs::metadata(&wal_path).map_or(0, |metadata| metadata.len());
    let stopped = Arc::new(AtomicBool::new(false));
    let writer_stopped = Arc::clone(&stopped);
    let ready = Arc::new(Barrier::new(2));
    let writer_ready = Arc::clone(&ready);
    let gain_record = records.first().expect("record").clone();
    let writer_database = database.clone();
    let writer = thread::spawn(move || {
        let writer = AnalysisCache::with_path(&writer_database);
        writer.open().expect("opens independent gain writer");
        let mut gain = Some(-3.0);
        let mut edit_count = 0_u64;
        let mut maximum_latency = Duration::ZERO;
        writer_ready.wait();
        let started = Instant::now();
        while !writer_stopped.load(Ordering::Relaxed) {
            let write_started = Instant::now();
            writer
                .store_custom_gain_for_record(&gain_record, gain)
                .expect("concurrent gain edit");
            maximum_latency = maximum_latency.max(write_started.elapsed());
            edit_count += 1;
            gain = if gain.is_some() { None } else { Some(-3.0) };
        }
        eprintln!(
            "Concurrent gains: {edit_count} writes over {:.3}s, maximum worker write latency {:.3} ms",
            started.elapsed().as_secs_f64(),
            maximum_latency.as_secs_f64() * 1000.0,
        );
    });
    ready.wait();
    group.bench_function("Cpp/concurrent_gain_writer", |bencher| {
        bencher.iter(|| retained_measurements(&cache, &records));
    });
    stopped.store(true, Ordering::Relaxed);
    writer.join().expect("gain writer exits");
    let wal_after = fs::metadata(&wal_path).map_or(0, |metadata| metadata.len());
    eprintln!("Shared cache WAL: {wal_before} bytes before reads, {wal_after} bytes after concurrent writes");
    group.finish();
}

/// Measures selected spectral hydration and publication without loading previews into folder measurements.
/// A committed short source always runs, and an optional generated high-rate track covers a much larger blob.
fn bench_spectrogram_cache(criterion: &mut Criterion) {
    let mut sources = vec![(
        "sine_stereo_44100_16.wav",
        test_fixtures::fixture("sine_stereo_44100_16.wav"),
    )];
    let long_name = "music_stereo_192000_24.wav";
    if let Some(path) = test_fixtures::benchmark_file(long_name) {
        sources.push((long_name, path));
    } else {
        eprintln!(
            "Skipping spectral cache {long_name}, generate it with uv run ../fixtures/benchmark/generate.py from rust/"
        );
    }

    let mut group = criterion.benchmark_group("cache/spectrogram");
    group.sample_size(10);
    group.warm_up_time(Duration::from_secs(1));
    group.measurement_time(Duration::from_secs(2));
    for (name, path) in sources {
        let directory = tempfile::tempdir().expect("creates isolated spectral cache directory");
        let database = directory.path().join("spectral.db");
        let cache = AnalysisCache::with_path(&database);
        let mut record = analyze_file(&path, &|| false, &mut |_| {});
        assert!(record.is_ready(), "{name}: {}", record.error_message);
        let data = record.spectrogram.take().expect("same-pass spectral artifact");
        record.waveform = None;
        cache
            .store_analysis(&record)
            .expect("stores measurements without previews");
        assert!(cache.get_spectrogram(&path).is_none());
        assert!(
            cache
                .store_spectrogram(&record, &data)
                .expect("publishes selected spectrum")
        );
        let loaded = cache.get_spectrogram(&path).expect("selected spectral hit");
        assert_eq!(loaded, data, "{name}: cache must preserve every spectral byte");
        assert!(loaded.matches_stream(record.sample_rate, record.length_in_samples, record.channels));
        let measurements = cache.get_analysis(&path).expect("measurements remain available");
        assert!(measurements.waveform.is_none() && measurements.spectrogram.is_none());
        let blob_bytes = data.to_bytes().len();
        let parameter = format!("{name}_{blob_bytes}bytes");
        group.throughput(Throughput::Bytes(blob_bytes as u64));
        group.bench_function(BenchmarkId::new("get_selected", &parameter), |bencher| {
            bencher.iter(|| black_box(cache.get_spectrogram(black_box(&path))));
        });
        group.bench_function(BenchmarkId::new("get_selected_new_connection", &parameter), |bencher| {
            bencher.iter_batched(
                || AnalysisCache::with_path(&database),
                |reader| {
                    let data = reader.get_spectrogram(black_box(&path));
                    // Keep connection teardown and loaded-grid destruction outside the measured operation.
                    black_box((reader, data))
                },
                BatchSize::PerIteration,
            );
        });
        group.bench_function(BenchmarkId::new("store_selected", &parameter), |bencher| {
            bencher.iter(|| {
                black_box(
                    cache
                        .store_spectrogram(black_box(&record), black_box(&data))
                        .expect("updates selected spectral artifact"),
                )
            });
        });
        group.throughput(Throughput::Elements(1));
        group.bench_function(
            BenchmarkId::new("measurements_without_previews", &parameter),
            |bencher| {
                bencher.iter(|| black_box(cache.get_analysis(black_box(&path))));
            },
        );
    }
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

criterion_group!(
    benches,
    bench_cache,
    bench_shared_cache,
    bench_spectrogram_cache,
    bench_sorting
);
criterion_main!(benches);
