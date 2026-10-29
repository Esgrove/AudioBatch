//! Benchmarks of normalization, the gain and AIFF writing alone, and `normalize_file` end to end.
//!
//! The render group applies the gain to in-memory music and writes it to an `AiffWriter` in memory,
//! at every output bit depth, so it measures our own sample loop and the float to integer conversion.
//! The end to end group normalizes a temporary copy of generated files in peak and loudness mode,
//! which includes the decode, the render, the metadata copy, the validation, and the analysis of the output.
//! Files that have not been generated with `fixtures/benchmark/generate.py` are skipped with a note.

// Benchmarks are test code, which the workspace allows to index, like the tests.
#![allow(clippy::indexing_slicing)]

use std::fs;
use std::hint::black_box;
use std::io::Cursor;
use std::path::Path;
use std::sync::Arc;
use std::time::Duration;

use audiobatch_core::aiff::AiffWriter;
use audiobatch_core::analyze_file;
use audiobatch_core::normalize::{NormalizationMode, NormalizationOptions, normalize_file};
use audiobatch_core::record::file_identity;
use audiobatch_core::signals::{SignalKind, generate};
use criterion::{BatchSize, BenchmarkId, Criterion, Throughput, criterion_group, criterion_main};

/// Length of the in-memory signal.
const SECONDS: usize = 10;

/// Frames per chunk, like a decoder packet.
const CHUNK_FRAMES: usize = 4_096;

/// Gain the render applies, about -3 dB.
const GAIN: f32 = 0.707;

/// Generated files for the end to end group.
const FILES: [&str; 4] = [
    "music_stereo_44100_16.wav",
    "music_stereo_44100_24.flac",
    "music_stereo_44100_320k.mp3",
    "music_stereo_96000_24.flac",
];

/// Scales the planes chunk by chunk and writes them as AIFF to memory, as the normalization render does.
fn render(planes: &[Vec<f32>], bits_per_sample: u32) -> usize {
    let frames = planes[0].len();
    let capacity = frames * planes.len() * 4 + 64;
    let cursor = Cursor::new(Vec::with_capacity(capacity));
    let mut writer = AiffWriter::new(cursor, planes.len() as u32, 44_100, bits_per_sample).expect("writer");
    let mut scaled: Vec<Vec<f32>> = vec![Vec::new(); planes.len()];
    let mut start = 0;
    while start < frames {
        let end = (start + CHUNK_FRAMES).min(frames);
        for (target, plane) in scaled.iter_mut().zip(planes) {
            target.clear();
            target.extend(plane[start..end].iter().map(|sample| sample * GAIN));
        }
        let chunk: Vec<&[f32]> = scaled.iter().map(Vec::as_slice).collect();
        writer.write_planar(&chunk).expect("writes");
        start = end;
    }
    writer.finish().expect("finishes").into_inner().len()
}

/// Benchmarks the gain and AIFF writing at every output bit depth.
fn bench_render(criterion: &mut Criterion) {
    let mut group = criterion.benchmark_group("render");
    group.sample_size(20);
    let frames = 44_100 * SECONDS;
    let planes = generate(SignalKind::MusicLike, 44_100, 2, frames);
    group.throughput(Throughput::Elements(frames as u64));
    for bits in [16, 24] {
        group.bench_with_input(BenchmarkId::new("gain_and_aiff", bits), &planes, |bencher, planes| {
            bencher.iter(|| render(black_box(planes), bits));
        });
    }
    group.finish();
}

/// Benchmarks `normalize_file` on a fresh temporary copy of each generated file, in both modes.
fn bench_end_to_end(criterion: &mut Criterion) {
    let mut group = criterion.benchmark_group("normalize_file");
    group.sample_size(10);
    group.measurement_time(Duration::from_secs(10));
    for name in FILES {
        let Some(path) = test_fixtures::benchmark_file(name) else {
            println!("Skipping {name}, run uv run fixtures/benchmark/generate.py to generate it");
            continue;
        };
        let source_record = analyze_file(&path, &|| false, &mut |_| {});
        group.throughput(Throughput::Elements(source_record.length_in_samples));
        for (label, mode) in [("peak", NormalizationMode::Peak), ("lufs", NormalizationMode::Loudness)] {
            let options = NormalizationOptions {
                mode,
                dispose: Arc::new(|path: &Path| fs::remove_file(path).is_ok()),
                ..NormalizationOptions::default()
            };
            group.bench_function(BenchmarkId::new(label, name), |bencher| {
                bencher.iter_batched(
                    || {
                        let directory = tempfile::tempdir().expect("creates directory");
                        let copy = directory.path().join(name);
                        fs::copy(&path, &copy).expect("copies the file");
                        let mut record = source_record.clone();
                        record.path.clone_from(&copy);
                        (record.file_size, record.modified_time_ms) = file_identity(&copy).expect("the copy exists");
                        (directory, record)
                    },
                    |(directory, record)| {
                        let result = normalize_file(&record, &options, &|| false, &mut |_| {});
                        assert_eq!(result.error(), None);
                        directory
                    },
                    BatchSize::PerIteration,
                );
            });
        }
    }
    group.finish();
}

criterion_group!(benches, bench_render, bench_end_to_end);
criterion_main!(benches);
