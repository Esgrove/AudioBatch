//! Benchmarks of the loudness and peak measurement, in memory and end to end on the generated files.
//!
//! The in-memory group measures each part of the meter alone on generated music,
//! so the time splits into true peak, K-weighting, short-term loudness, and our own block handling.
//! The `ebur128` groups feed the reference meter the way the analysis did before our own meter,
//! so the old and the new path compare side by side in one report.
//! The end to end group runs `analyze_file` on the files of `fixtures/benchmark/generate.py`,
//! and skips with a note when they have not been generated.
//! Throughput is reported in frames, so the results read as frames per second.

// Benchmarks are test code, which the workspace allows to index, like the tests.
#![allow(clippy::indexing_slicing)]

use std::hint::black_box;
use std::time::Duration;

use audiobatch_core::analyze_file;
use audiobatch_core::loudness::true_peak::TruePeak;
use audiobatch_core::meter::BlockMeter;
use audiobatch_core::signals::{SignalKind, generate};
use criterion::{BenchmarkId, Criterion, Throughput, criterion_group, criterion_main};
use ebur128::{Channel, EbuR128, Mode};

/// Length of the in-memory signals, long enough to fill the short-term window many times over.
const SECONDS: usize = 10;

/// Rates around every change of the true peak oversampling.
const RATES: [u32; 5] = [44_100, 48_000, 88_200, 96_000, 192_000];

/// Frames per chunk fed to the meters, like a decoder packet.
const CHUNK_FRAMES: usize = 4_096;

/// Generated files for the end to end group, the common formats at 44.1 kHz and every rate as 24 bit WAV.
const FILES: [&str; 11] = [
    "music_stereo_44100_16.wav",
    "music_stereo_44100_16.flac",
    "music_stereo_44100_320k.mp3",
    "music_stereo_44100_256k.m4a",
    "music_stereo_44100_q5.ogg",
    "music_mono_44100_16.wav",
    "music_stereo_48000_24.wav",
    "music_stereo_88200_24.wav",
    "music_stereo_96000_24.wav",
    "music_stereo_192000_24.wav",
    "noise_stereo_44100_24.flac",
];

/// Feeds the planes to ebur128 in 100 ms blocks with the mode, reading the short-term loudness after each block when asked.
fn run_ebur128(planes: &[Vec<f32>], sample_rate: u32, mode: Mode, read_short_term: bool) -> f64 {
    let mut meter = EbuR128::new(planes.len() as u32, sample_rate, mode).expect("meter");
    if planes.len() == 1 {
        meter.set_channel(0, Channel::DualMono).expect("dual mono");
    }
    let block = (f64::from(sample_rate) / 10.0).round() as usize;
    let frames = planes[0].len();
    let mut total = 0.0;
    let mut start = 0;
    while start < frames {
        let end = (start + block).min(frames);
        let chunk: Vec<&[f32]> = planes.iter().map(|plane| &plane[start..end]).collect();
        meter.add_frames_planar_f32(&chunk).expect("frames");
        if read_short_term {
            total += meter.loudness_shortterm().expect("short-term");
        }
        start = end;
    }
    total
}

/// Measures the planes with the full block meter, as the analysis does after decoding.
fn run_block_meter(planes: &[Vec<f32>], sample_rate: u32) -> f64 {
    let frames = planes[0].len();
    let mut meter = BlockMeter::new(planes.len() as u32, sample_rate, Some(frames as u64)).expect("meter");
    let mut start = 0;
    while start < frames {
        let end = (start + CHUNK_FRAMES).min(frames);
        let chunk: Vec<&[f32]> = planes.iter().map(|plane| &plane[start..end]).collect();
        meter.push(&chunk).expect("push");
        start = end;
    }
    meter.finish(frames as u64).expect("finish").integrated_lufs
}

/// Measures independent interpolation phases in SIMD lanes without decoding or other loudness work.
fn run_true_peak(planes: &[Vec<f32>], sample_rate: u32) -> f64 {
    let mut meter = TruePeak::new(planes.len(), sample_rate);
    let frames = planes[0].len();
    let mut start = 0;
    while start < frames {
        let end = (start + CHUNK_FRAMES).min(frames);
        let chunk: Vec<&[f32]> = planes.iter().map(|plane| &plane[start..end]).collect();
        meter.push(&chunk);
        start = end;
    }
    meter.peaks().iter().sum()
}

/// Benchmarks every part of the measurement on in-memory stereo and mono music at every rate.
fn bench_in_memory(criterion: &mut Criterion) {
    let mut group = criterion.benchmark_group("meter");
    group.sample_size(10);
    group.measurement_time(Duration::from_secs(4));
    for channels in [2, 1] {
        for rate in RATES {
            if channels == 1 && rate != 44_100 {
                continue;
            }
            let frames = rate as usize * SECONDS;
            let planes = generate(SignalKind::MusicLike, rate, channels, frames);
            let parameter = format!("{rate}_{channels}ch");
            group.throughput(Throughput::Elements(frames as u64));
            group.bench_with_input(
                BenchmarkId::new("block_meter", &parameter),
                &planes,
                |bencher, planes| {
                    bencher.iter(|| run_block_meter(black_box(planes), rate));
                },
            );
            group.bench_with_input(
                BenchmarkId::new("simd_true_peak", &parameter),
                &planes,
                |bencher, planes| {
                    bencher.iter(|| run_true_peak(black_box(planes), rate));
                },
            );
            let parts = [
                ("ebur128_full", Mode::I | Mode::S | Mode::TRUE_PEAK, true),
                ("ebur128_true_peak", Mode::TRUE_PEAK, false),
                ("ebur128_k_weighting", Mode::I | Mode::S, false),
                ("ebur128_short_term", Mode::I | Mode::S, true),
            ];
            for (name, mode, read_short_term) in parts {
                group.bench_with_input(BenchmarkId::new(name, &parameter), &planes, |bencher, planes| {
                    bencher.iter(|| run_ebur128(black_box(planes), rate, mode, read_short_term));
                });
            }
        }
    }
    group.finish();
}

/// Benchmarks `analyze_file` on the generated files, decode and measurement together.
fn bench_end_to_end(criterion: &mut Criterion) {
    let mut group = criterion.benchmark_group("analyze_file");
    group.sample_size(10);
    group.measurement_time(Duration::from_secs(8));
    for name in FILES {
        let Some(path) = test_fixtures::benchmark_file(name) else {
            println!("Skipping {name}, run uv run fixtures/benchmark/generate.py to generate it");
            continue;
        };
        let record = analyze_file(&path, &|| false, &mut |_| {});
        assert!(record.is_ready(), "{name}: {}", record.error_message);
        group.throughput(Throughput::Elements(record.length_in_samples));
        group.bench_with_input(BenchmarkId::from_parameter(name), &path, |bencher, path| {
            bencher.iter(|| analyze_file(black_box(path), &|| false, &mut |_| {}));
        });
    }
    group.finish();
}

criterion_group!(benches, bench_in_memory, bench_end_to_end);
criterion_main!(benches);
