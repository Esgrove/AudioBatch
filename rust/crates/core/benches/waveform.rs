//! Benchmarks of the waveform overview: building it from samples, its cache blob, and the columns the GUI draws.
//!
//! The overview is built in the same pass as the measurement, so `push` runs for every decoded sample,
//! while `column_peaks` runs on every repaint of the waveform at the width of the view.

// Benchmarks are test code, which the workspace allows to index, like the tests.
#![allow(clippy::indexing_slicing)]

use std::hint::black_box;
use std::sync::Arc;

use audiobatch_core::signals::{SignalKind, generate};
use audiobatch_core::waveform::{WaveformBuilder, WaveformOverview};
use criterion::{BenchmarkId, Criterion, Throughput, criterion_group, criterion_main};

/// Sample rate of the signal.
const SAMPLE_RATE: u32 = 44_100;

/// Length of the signal, a typical track.
const SECONDS: usize = 240;

/// Frames per chunk, like a decoder packet.
const CHUNK_FRAMES: usize = 4_096;

/// Builds the overview of the planes chunk by chunk, as the analysis does.
fn build(planes: &[Vec<f32>]) -> Arc<WaveformOverview> {
    let frames = planes[0].len();
    let mut builder = WaveformBuilder::new(planes.len(), Some(frames as u64));
    let mut start = 0;
    while start < frames {
        let end = (start + CHUNK_FRAMES).min(frames);
        let chunk: Vec<&[f32]> = planes.iter().map(|plane| &plane[start..end]).collect();
        builder.push(&chunk);
        start = end;
    }
    builder.finish(SAMPLE_RATE, frames as u64)
}

/// Benchmarks building, serializing, and drawing the overview of a stereo track.
fn bench_waveform(criterion: &mut Criterion) {
    let frames = SAMPLE_RATE as usize * SECONDS;
    let planes = generate(SignalKind::MusicLike, SAMPLE_RATE, 2, frames);
    let overview = build(&planes);
    let blob = overview.to_bytes();

    let mut group = criterion.benchmark_group("waveform");
    group.sample_size(20);
    group.throughput(Throughput::Elements(frames as u64));
    group.bench_function("push_and_finish", |bencher| {
        bencher.iter(|| build(black_box(&planes)));
    });
    group.throughput(Throughput::Bytes(blob.len() as u64));
    group.bench_function("to_bytes", |bencher| {
        bencher.iter(|| black_box(&overview).to_bytes());
    });
    group.bench_function("from_bytes", |bencher| {
        bencher.iter(|| WaveformOverview::from_bytes(black_box(&blob)));
    });
    for columns in [800, 1600, 3200] {
        group.throughput(Throughput::Elements(columns as u64));
        group.bench_with_input(
            BenchmarkId::new("column_peaks_full", columns),
            &columns,
            |bencher, &columns| {
                bencher.iter(|| black_box(&overview).column_peaks(0, 0.0, frames as f64, columns));
            },
        );
        group.bench_with_input(
            BenchmarkId::new("column_peaks_zoomed", columns),
            &columns,
            |bencher, &columns| {
                bencher.iter(|| black_box(&overview).column_peaks(1, 1_000_000.0, 1_441_000.0, columns));
            },
        );
    }
    group.finish();
}

criterion_group!(benches, bench_waveform);
criterion_main!(benches);
