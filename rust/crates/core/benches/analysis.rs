//! Benchmarks of the loudness and peak measurement, in memory and end to end on the generated files.
//!
//! The in-memory group measures the complete production meter and standalone SIMD true peak on generated music.
//! The `ebur128` groups feed the development reference in 100 ms blocks,
//! comparing its complete meter, true peak, K weighting, and short-term queries in the same report.
//! The underflow group measures fresh interpolation histories at ordinary, tiny normal, and subnormal amplitudes.
//! The overload group isolates the production flag collector, including stride doubling and final partial buckets.
//! The end to end group runs `analyze_file` on the files of `fixtures/benchmark/generate.py`,
//! and skips with a note when they have not been generated.
//! Throughput is reported in frames, so the results read as frames per second.

// Benchmarks are test code, which the workspace allows to index, like the tests.
#![expect(
    clippy::indexing_slicing,
    reason = "benchmarks are test code, where indexing is allowed"
)]

use std::hint::black_box;
use std::time::Duration;

use audiobatch_core::analyze_file;
use audiobatch_core::loudness::true_peak::TruePeak;
use audiobatch_core::meter::BlockMeter;
use audiobatch_core::overload::{OverloadHistoryBuilder, SAMPLE_OVER, TRUE_PEAK_OVER};
use audiobatch_core::signals::{SignalKind, generate};
use criterion::{BatchSize, BenchmarkId, Criterion, Throughput, criterion_group, criterion_main};
use ebur128::{Channel, EbuR128, Mode};

/// Length of the in-memory signals, long enough to fill the short-term window many times over.
const SECONDS: usize = 10;

/// Rates around every change of the true peak oversampling.
const RATES: [u32; 5] = [44_100, 48_000, 88_200, 96_000, 192_000];

/// Frames per chunk fed to the meters, like a decoder packet.
const CHUNK_FRAMES: usize = 4096;

/// Generated files for the end to end group, the common formats at 44.1 kHz and every rate as 24 bit WAV.
const FILES: [&str; 10] = [
    "music_stereo_44100_16.wav",
    "music_stereo_44100_16.flac",
    "music_stereo_44100_320k.mp3",
    "music_stereo_44100_256k.m4a",
    "music_mono_44100_16.wav",
    "music_stereo_48000_24.wav",
    "music_stereo_88200_24.wav",
    "music_stereo_96000_24.wav",
    "music_stereo_192000_24.wav",
    "noise_stereo_44100_24.flac",
];

/// Borrows a mono or stereo chunk in fixed storage, without a harness allocation in the measured loop.
fn chunk_planes(planes: &[Vec<f32>], start: usize, end: usize) -> [&[f32]; 2] {
    assert!((1..=2).contains(&planes.len()), "benchmark signals are mono or stereo");
    [
        &planes[0][start..end],
        planes.get(1).map_or(&[][..], |plane| &plane[start..end]),
    ]
}

/// Feeds the planes to ebur128 in 100 ms blocks, returning every requested measurement.
fn run_ebur128(planes: &[Vec<f32>], sample_rate: u32, mode: Mode, read_short_term: bool) -> [f64; 4] {
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
        let chunk = chunk_planes(planes, start, end);
        let chunk = &chunk[..planes.len()];
        meter.add_frames_planar_f32(chunk).expect("frames");
        if read_short_term {
            total += black_box(meter.loudness_shortterm().expect("short-term"));
        }
        start = end;
    }
    let integrated = if mode.contains(Mode::I) {
        meter.loudness_global().expect("integrated")
    } else {
        0.0
    };
    let mut peaks = [0.0; 2];
    if mode.contains(Mode::TRUE_PEAK) {
        for (channel, peak) in peaks.iter_mut().take(planes.len()).enumerate() {
            *peak = meter.true_peak(channel as u32).expect("true peak");
        }
    }
    [integrated, total, peaks[0], peaks[1]]
}

/// Measures the planes with the full block meter, as the analysis does after decoding.
fn run_block_meter(planes: &[Vec<f32>], sample_rate: u32) -> f64 {
    let frames = planes[0].len();
    let mut meter = BlockMeter::new(planes.len() as u32, sample_rate, Some(frames as u64)).expect("meter");
    let mut start = 0;
    while start < frames {
        let end = (start + CHUNK_FRAMES).min(frames);
        let chunk = chunk_planes(planes, start, end);
        let chunk = &chunk[..planes.len()];
        meter.push(chunk);
        start = end;
    }
    black_box(meter.finish(frames as u64)).integrated_lufs
}

/// Measures independent interpolation phases in SIMD lanes without decoding or other loudness work.
fn run_true_peak(planes: &[Vec<f32>], sample_rate: u32) -> f64 {
    let mut meter = TruePeak::new(planes.len(), sample_rate);
    let frames = planes[0].len();
    let mut start = 0;
    while start < frames {
        let end = (start + CHUNK_FRAMES).min(frames);
        let chunk = chunk_planes(planes, start, end);
        let chunk = &chunk[..planes.len()];
        meter.push(chunk);
        start = end;
    }
    meter.peaks().iter().sum()
}

/// Benchmarks the production meter, SIMD true peak, and reference components on in-memory music.
/// Stereo covers every configured rate, while mono covers 44.1 kHz.
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
                    bencher.iter(|| black_box(run_block_meter(black_box(planes), rate)));
                },
            );
            group.bench_with_input(
                BenchmarkId::new("simd_true_peak", &parameter),
                &planes,
                |bencher, planes| {
                    bencher.iter(|| black_box(run_true_peak(black_box(planes), rate)));
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
                    bencher.iter(|| black_box(run_ebur128(black_box(planes), rate, mode, read_short_term)));
                });
            }
        }
    }
    group.finish();
}

/// Isolates repeated meter construction and short pushes whose setup cost can dominate interpolation.
fn bench_true_peak_setup(criterion: &mut Criterion) {
    let mut group = criterion.benchmark_group("true_peak_setup");
    group.sample_size(20);
    group.measurement_time(Duration::from_secs(3));
    for rate in [48_000, 96_000] {
        group.bench_function(BenchmarkId::new("construct", rate), |bencher| {
            bencher.iter(|| black_box(TruePeak::new(black_box(2), black_box(rate))));
        });
        for frames in [1, 12, 64] {
            let samples = vec![0.5; frames];
            let planes = [samples.as_slice(); 2];
            group.bench_function(format!("push/{rate}/{frames}"), |bencher| {
                bencher.iter_batched(
                    || TruePeak::new(2, rate),
                    |mut meter| {
                        for _ in 0..512 {
                            meter.push(black_box(&planes));
                        }
                        black_box(meter.peaks());
                    },
                    BatchSize::SmallInput,
                );
            });
        }
    }
    group.finish();
}

/// Measures underflowing interpolation from an empty history, without an earlier loud peak masking the result.
fn bench_true_peak_underflow(criterion: &mut Criterion) {
    let mut group = criterion.benchmark_group("true_peak_underflow");
    group.sample_size(10);
    group.measurement_time(Duration::from_secs(3));
    let rate = 48_000;
    let frames = 4800;
    group.throughput(Throughput::Elements(frames as u64));
    for (name, amplitude) in [
        ("ordinary", 0.5),
        ("small_normal", f32::MIN_POSITIVE * 4.0),
        ("subnormal", f32::MIN_POSITIVE * 0.5),
    ] {
        let planes = vec![
            (0..frames)
                .map(|frame| if frame % 4 < 2 { amplitude } else { -amplitude })
                .collect(),
        ];
        group.bench_with_input(BenchmarkId::new("simd", name), &planes, |bencher, planes| {
            bencher.iter(|| black_box(run_true_peak(black_box(planes), rate)));
        });
        group.bench_with_input(BenchmarkId::new("ebur128", name), &planes, |bencher, planes| {
            bencher.iter(|| black_box(run_ebur128(black_box(planes), rate, Mode::TRUE_PEAK, false)));
        });
    }
    group.finish();
}

/// Measures the production flag collector alone, including bounded merging and the final partial bucket.
fn bench_overload_history(criterion: &mut Criterion) {
    let mut group = criterion.benchmark_group("overload_history");
    let rate = 48_000;
    let block = 4800;
    for channels in [1, 2, 8, 64] {
        let flags: Vec<_> = (0..channels)
            .map(|channel| if channel % 2 == 0 { SAMPLE_OVER } else { TRUE_PEAK_OVER })
            .collect();
        for blocks in [100_u64, 8193, 32_771] {
            group.throughput(Throughput::Elements(blocks));
            group.bench_function(format!("{channels}ch_{blocks}blocks"), |bencher| {
                bencher.iter(|| {
                    let mut builder = OverloadHistoryBuilder::new(channels, rate);
                    for _ in 0..blocks {
                        builder.push(black_box(&flags));
                    }
                    black_box(builder.finish((blocks - 1) * block + 7))
                });
            });
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

criterion_group!(
    benches,
    bench_in_memory,
    bench_true_peak_setup,
    bench_true_peak_underflow,
    bench_overload_history,
    bench_end_to_end
);
criterion_main!(benches);
