//! Bounded spectrogram construction, EOF tail quantization, portable blobs, and streaming preview backfill.
//!
//! Signal generation and exact geometry, chunk-invariance, and codec preflights run outside timing.
//! Construction includes per-builder FFT planning and allocations, with the process-wide Hann window already initialized.
//! Separate push groups include or exclude construction, and EOF benchmarks prepare production builders before timing.
//! EOF finishing includes the remaining padded FFTs and their quantization, not quantization of the whole grid.
//! The merge case repeats a fixed PCM block past two stride doublings without retaining a long source.
//! Frame throughput counts source frames, EOF throughput counts remaining windows,
//! and codec throughput counts blob bytes.
//! Optional file cases stream decode into both preview builders without invoking a measurement meter.

#![expect(
    clippy::indexing_slicing,
    reason = "benchmarks are test code, where indexing is allowed"
)]

use std::hint::black_box;
use std::path::Path;
use std::sync::Arc;
use std::time::Duration;

use audiobatch_core::decode::AudioFileDecoder;
use audiobatch_core::signals::{SignalKind, generate};
use audiobatch_core::spectrogram::{
    FFT_SIZE, FREQUENCY_ROWS, HOP_SIZE, MAXIMUM_COLUMNS, SpectrogramBuilder, SpectrogramData,
};
use audiobatch_core::waveform::{WaveformBuilder, WaveformOverview};
use audiobatch_core::{AnalysisCache, AnalysisRecord, analyze_file};
use criterion::{BatchSize, BenchmarkId, Criterion, SamplingMode, Throughput, criterion_group, criterion_main};
use rusqlite::Connection;

/// Decoder-like aligned chunk size, also bounding retained PCM in the merge case.
const CHUNK_FRAMES: usize = 4096;
/// Prime-sized chunks deliberately cross FFT and hop boundaries.
const IRREGULAR_CHUNK_FRAMES: usize = 997;
/// Short in-memory cases cover many FFTs without making the complete suite a long-running DSP soak.
const SIGNAL_FRAMES: usize = 64 * HOP_SIZE;
/// A non-hop-aligned EOF exercises the final partial column and zero-padded tail.
const FINAL_PARTIAL_FRAMES: usize = 17;
/// Crossing both the 4096th and 8192nd hop centers forces repeated bounded-grid merges.
const MERGE_FRAMES: usize = 2 * MAXIMUM_COLUMNS * HOP_SIZE + FINAL_PARTIAL_FRAMES;
/// Generated files cover uncompressed, lossless-compressed, and lossy backfill without a codec/rate Cartesian product.
const BACKFILL_FILES: [&str; 3] = [
    "music_stereo_44100_16.wav",
    "music_stereo_96000_24.flac",
    "music_stereo_44100_320k.mp3",
];

/// One selected geometry and signal, with its own chunk and EOF boundary contract.
struct SignalCase {
    kind: SignalKind,
    rate: u32,
    channels: usize,
    frames: usize,
    chunk_frames: usize,
}

impl SignalCase {
    /// Names all input properties so saved Criterion baselines remain interpretable.
    fn id(&self) -> String {
        format!(
            "{}_{}_{}ch_{}frames_chunk{}",
            self.kind.name(),
            self.rate,
            self.channels,
            self.frames,
            self.chunk_frames,
        )
    }
}

/// Returns a sparse matrix covering mono and stereo at each representative source rate.
const fn signal_cases() -> [SignalCase; 10] {
    [
        SignalCase {
            kind: SignalKind::MusicLike,
            rate: 44_100,
            channels: 1,
            frames: SIGNAL_FRAMES,
            chunk_frames: CHUNK_FRAMES,
        },
        SignalCase {
            kind: SignalKind::MusicLike,
            rate: 44_100,
            channels: 2,
            frames: SIGNAL_FRAMES + FINAL_PARTIAL_FRAMES,
            chunk_frames: IRREGULAR_CHUNK_FRAMES,
        },
        SignalCase {
            kind: SignalKind::MusicLike,
            rate: 48_000,
            channels: 1,
            frames: SIGNAL_FRAMES + FINAL_PARTIAL_FRAMES,
            chunk_frames: IRREGULAR_CHUNK_FRAMES,
        },
        SignalCase {
            kind: SignalKind::MusicLike,
            rate: 48_000,
            channels: 2,
            frames: SIGNAL_FRAMES + FINAL_PARTIAL_FRAMES,
            chunk_frames: CHUNK_FRAMES,
        },
        SignalCase {
            kind: SignalKind::MusicLike,
            rate: 96_000,
            channels: 1,
            frames: SIGNAL_FRAMES,
            chunk_frames: CHUNK_FRAMES,
        },
        SignalCase {
            kind: SignalKind::MusicLike,
            rate: 96_000,
            channels: 2,
            frames: SIGNAL_FRAMES + FINAL_PARTIAL_FRAMES,
            chunk_frames: IRREGULAR_CHUNK_FRAMES,
        },
        SignalCase {
            kind: SignalKind::MusicLike,
            rate: 192_000,
            channels: 1,
            frames: SIGNAL_FRAMES + FINAL_PARTIAL_FRAMES,
            chunk_frames: IRREGULAR_CHUNK_FRAMES,
        },
        SignalCase {
            kind: SignalKind::MusicLike,
            rate: 192_000,
            channels: 2,
            frames: SIGNAL_FRAMES,
            chunk_frames: CHUNK_FRAMES,
        },
        SignalCase {
            kind: SignalKind::Silence,
            rate: 48_000,
            channels: 2,
            frames: SIGNAL_FRAMES + FINAL_PARTIAL_FRAMES,
            chunk_frames: CHUNK_FRAMES,
        },
        SignalCase {
            kind: SignalKind::Noise,
            rate: 48_000,
            channels: 2,
            frames: SIGNAL_FRAMES + FINAL_PARTIAL_FRAMES,
            chunk_frames: IRREGULAR_CHUNK_FRAMES,
        },
    ]
}

/// Borrows mono or stereo chunks with fixed plane-reference storage and no per-push harness allocation.
fn chunk_planes(planes: &[Vec<f32>], start: usize, end: usize) -> [&[f32]; 2] {
    [
        &planes[0][start..end],
        planes.get(1).map_or(&[][..], |plane| &plane[start..end]),
    ]
}

/// Feeds consecutive short input planes into a production builder supplied by the caller.
fn push_planes(builder: &mut SpectrogramBuilder, planes: &[Vec<f32>], chunk_frames: usize) {
    for start in (0..planes[0].len()).step_by(chunk_frames) {
        let end = (start + chunk_frames).min(planes[0].len());
        let chunk = chunk_planes(planes, start, end);
        builder.push(black_box(&chunk[..planes.len()]));
    }
}

/// Constructs and feeds a builder without finishing it, also serving as untimed EOF preparation.
fn prepare(planes: &[Vec<f32>], rate: u32, chunk_frames: usize) -> SpectrogramBuilder {
    let mut builder = SpectrogramBuilder::new(planes.len(), rate);
    push_planes(&mut builder, planes, chunk_frames);
    builder
}

/// Repeats a fixed block while preserving sample order under either aligned or irregular chunk boundaries.
fn push_repeating(builder: &mut SpectrogramBuilder, planes: &[Vec<f32>], frames: usize, chunk_frames: usize) {
    let mut remaining = frames;
    while remaining > 0 {
        let block_frames = remaining.min(planes[0].len());
        for start in (0..block_frames).step_by(chunk_frames) {
            let end = (start + chunk_frames).min(block_frames);
            let chunk = chunk_planes(planes, start, end);
            builder.push(black_box(&chunk[..planes.len()]));
        }
        remaining -= block_frames;
    }
}

/// Checks canonical bounded rows and columns independently of the builder and round-trips every byte exactly.
fn check_data(data: &SpectrogramData, rate: u32, channels: usize, frames: usize) {
    let mut stride = HOP_SIZE as u64;
    while (frames as u64).div_ceil(stride) > MAXIMUM_COLUMNS as u64 {
        stride *= 2;
    }
    let columns = (frames as u64).div_ceil(stride) as usize;
    assert!(data.matches_stream(rate, frames as u64, channels as u32));
    assert_eq!(data.samples_per_column(), stride);
    assert_eq!(data.column_count(), columns);
    assert_eq!(data.intensities().len(), columns * FREQUENCY_ROWS);
    assert!(columns <= MAXIMUM_COLUMNS);
    assert_eq!(SpectrogramData::from_bytes(&data.to_bytes()).as_ref(), Some(data));
}

/// Ensures timed signal cases cannot silently benchmark invalid, noncanonical, or chunk-dependent results.
fn preflight(case: &SignalCase, planes: &[Vec<f32>]) {
    let data = prepare(planes, case.rate, case.chunk_frames)
        .finish()
        .expect("valid signal spectrogram");
    check_data(&data, case.rate, case.channels, case.frames);
    let other_chunk = if case.chunk_frames == CHUNK_FRAMES {
        IRREGULAR_CHUNK_FRAMES
    } else {
        CHUNK_FRAMES
    };
    let other = prepare(planes, case.rate, other_chunk)
        .finish()
        .expect("chunk-invariant signal spectrogram");
    assert_eq!(data, other, "{}", case.id());
    if case.kind == SignalKind::Silence {
        assert!(data.intensities().iter().all(|&intensity| intensity == 0));
    } else {
        assert!(data.intensities().iter().any(|&intensity| intensity > 0));
    }
}

/// Measures per-builder planning, row support, and allocation separately from shared one-time Hann initialization.
fn bench_setup(criterion: &mut Criterion) {
    // There is no public reset for the immutable process-wide window.
    // Prime it explicitly rather than mislabel repeated constructor timings as cold-process initialization.
    drop(SpectrogramBuilder::new(1, 44_100));
    let mut group = criterion.benchmark_group("spectrogram/setup");
    group.sample_size(10);
    group.warm_up_time(Duration::from_secs(1));
    group.measurement_time(Duration::from_secs(2));
    group.throughput(Throughput::Elements(1));
    for (channels, rate) in [(1, 44_100), (2, 48_000), (2, 96_000), (2, 192_000)] {
        group.bench_function(BenchmarkId::new("new", format!("{rate}_{channels}ch")), |bencher| {
            bencher.iter_batched(
                || (),
                |()| black_box(SpectrogramBuilder::new(black_box(channels), black_box(rate))),
                BatchSize::PerIteration,
            );
        });
    }
    group.finish();
}

/// Separates complete construction plus processing from processing with a newly prepared, untimed builder.
fn bench_push(criterion: &mut Criterion) {
    let mut group = criterion.benchmark_group("spectrogram/push");
    group.sample_size(10);
    group.warm_up_time(Duration::from_secs(1));
    group.measurement_time(Duration::from_secs(2));
    for case in signal_cases() {
        let planes = generate(case.kind, case.rate, case.channels, case.frames);
        preflight(&case, &planes);
        let parameter = case.id();
        group.throughput(Throughput::Elements(case.frames as u64));
        group.bench_function(BenchmarkId::new("new_push_finish", &parameter), |bencher| {
            bencher.iter_batched(
                || (),
                |()| black_box(prepare(black_box(&planes), case.rate, case.chunk_frames).finish()),
                BatchSize::PerIteration,
            );
        });
        group.bench_function(BenchmarkId::new("push_finish", &parameter), |bencher| {
            bencher.iter_batched(
                || SpectrogramBuilder::new(case.channels, case.rate),
                |mut builder| {
                    push_planes(&mut builder, black_box(&planes), case.chunk_frames);
                    black_box(builder.finish())
                },
                BatchSize::PerIteration,
            );
        });
    }
    group.finish();
}

/// Times only EOF work after all supplied PCM has been pushed outside the measured closure.
fn bench_finish(criterion: &mut Criterion) {
    let mut group = criterion.benchmark_group("spectrogram/finish_tail_windows");
    group.sample_size(10);
    group.warm_up_time(Duration::from_secs(1));
    group.measurement_time(Duration::from_secs(2));
    for (channels, rate, frames) in [
        (1, 44_100, 1),
        (2, 48_000, FFT_SIZE / 2 + FINAL_PARTIAL_FRAMES),
        (2, 96_000, SIGNAL_FRAMES + FINAL_PARTIAL_FRAMES),
    ] {
        let case = SignalCase {
            kind: SignalKind::Noise,
            rate,
            channels,
            frames,
            chunk_frames: IRREGULAR_CHUNK_FRAMES,
        };
        let planes = generate(case.kind, rate, channels, frames);
        preflight(&case, &planes);
        let pushed_windows = if frames < FFT_SIZE / 2 {
            0
        } else {
            (frames - FFT_SIZE / 2) / HOP_SIZE + 1
        };
        let tail_windows = frames.div_ceil(HOP_SIZE) - pushed_windows;
        group.throughput(Throughput::Elements(tail_windows as u64));
        group.bench_function(BenchmarkId::new("finish", case.id()), |bencher| {
            bencher.iter_batched(
                || prepare(&planes, rate, case.chunk_frames),
                |builder| black_box(builder.finish()),
                BatchSize::PerIteration,
            );
        });
    }
    group.finish();
}

/// Exercises two in-place maximum merges with bounded input storage and a final partial column.
fn bench_merge(criterion: &mut Criterion) {
    let rate = 96_000;
    let planes = generate(SignalKind::MusicLike, rate, 2, CHUNK_FRAMES);
    let mut builder = SpectrogramBuilder::new(2, rate);
    push_repeating(&mut builder, &planes, MERGE_FRAMES, CHUNK_FRAMES);
    let data = builder.finish().expect("repeated-merge spectrogram");
    check_data(&data, rate, 2, MERGE_FRAMES);
    assert_eq!(data.samples_per_column(), 4 * HOP_SIZE as u64);
    let mut other = SpectrogramBuilder::new(2, rate);
    push_repeating(&mut other, &planes, MERGE_FRAMES, IRREGULAR_CHUNK_FRAMES);
    assert_eq!(Some(data), other.finish(), "merge output must be chunk-invariant");

    let mut group = criterion.benchmark_group("spectrogram/merge");
    group.sample_size(10);
    group.warm_up_time(Duration::from_secs(1));
    group.measurement_time(Duration::from_secs(5));
    group.sampling_mode(SamplingMode::Flat);
    group.throughput(Throughput::Elements(MERGE_FRAMES as u64));
    group.bench_function(
        format!("push_finish/music_{rate}_2ch_{MERGE_FRAMES}frames_chunk{CHUNK_FRAMES}"),
        |bencher| {
            bencher.iter_batched(
                || SpectrogramBuilder::new(2, rate),
                |mut builder| {
                    push_repeating(&mut builder, black_box(&planes), MERGE_FRAMES, CHUNK_FRAMES);
                    black_box(builder.finish())
                },
                BatchSize::PerIteration,
            );
        },
    );
    group.finish();
}

/// Measures exact portable blob encoding and decoding for tiny, track-sized, and maximum-width artifacts.
fn bench_codec(criterion: &mut Criterion) {
    let rate = 48_000;
    let planes = generate(SignalKind::Noise, rate, 2, CHUNK_FRAMES);
    let mut group = criterion.benchmark_group("spectrogram/codec");
    group.sample_size(10);
    group.warm_up_time(Duration::from_secs(1));
    group.measurement_time(Duration::from_secs(2));
    for columns in [1, 512, MAXIMUM_COLUMNS] {
        let frames = (columns - 1) * HOP_SIZE + 1;
        let mut builder = SpectrogramBuilder::new(2, rate);
        push_repeating(&mut builder, &planes, frames, CHUNK_FRAMES);
        let data = builder.finish().expect("codec source spectrogram");
        check_data(&data, rate, 2, frames);
        let bytes = data.to_bytes();
        let parameter = format!("{columns}columns_{}bytes", bytes.len());
        group.throughput(Throughput::Bytes(bytes.len() as u64));
        group.bench_function(BenchmarkId::new("to_bytes", &parameter), |bencher| {
            bencher.iter(|| black_box(black_box(&data).to_bytes()));
        });
        group.bench_function(BenchmarkId::new("from_bytes", &parameter), |bencher| {
            bencher.iter(|| black_box(SpectrogramData::from_bytes(black_box(&bytes))));
        });
    }
    group.finish();
}

/// Bounded preview artifacts decoded together outside measurement-only cache reads.
struct DecodedPreviews {
    waveform: Arc<WaveformOverview>,
    spectrogram: Arc<SpectrogramData>,
}

/// Streams one source through production preview builders, retaining only the current decoder packet and bounded grids.
fn decode_previews(path: &Path) -> DecodedPreviews {
    let mut decoder = AudioFileDecoder::open(path).expect("opens backfill source");
    let info = decoder.info().clone();
    let channels = info.channels as usize;
    assert!(
        (1..=2).contains(&channels),
        "backfill benchmark sources are mono or stereo"
    );
    let mut waveform = WaveformBuilder::new(channels, info.expected_frames);
    let mut spectrogram = SpectrogramBuilder::new(channels, info.sample_rate);
    while let Some(planes) = decoder.next_chunk().expect("decodes backfill packet") {
        let chunk = chunk_planes(planes, 0, planes[0].len());
        let chunk = &chunk[..channels];
        waveform.push(chunk);
        spectrogram.push(chunk);
    }
    let frames = decoder.position();
    DecodedPreviews {
        waveform: waveform.finish(info.sample_rate, frames),
        spectrogram: spectrogram.finish().expect("valid backfill spectrogram"),
    }
}

/// Clears only optional artifacts outside timing so every measured publication fills a real cache miss.
fn clear_previews(connection: &Connection, record: &AnalysisRecord) {
    let key = record.path.to_string_lossy();
    connection
        .execute("DELETE FROM rust_waveform_cache WHERE file_path = ?1", [key.as_ref()])
        .expect("clears waveform before backfill");
    connection
        .execute("DELETE FROM spectrogram_cache WHERE file_path = ?1", [key.as_ref()])
        .expect("clears spectrogram before backfill");
}

/// Compares streaming-only and persisted combined backfill, independently of the GUI admission queue or meters.
fn bench_backfill(criterion: &mut Criterion) {
    let mut group = criterion.benchmark_group("spectrogram/preview_backfill");
    group.sample_size(10);
    group.warm_up_time(Duration::from_secs(1));
    group.measurement_time(Duration::from_secs(2));
    group.sampling_mode(SamplingMode::Flat);
    for name in BACKFILL_FILES {
        let Some(path) = test_fixtures::benchmark_file(name) else {
            eprintln!("Skipping {name}, generate it with uv run ../fixtures/benchmark/generate.py from rust/");
            continue;
        };
        let mut record = analyze_file(&path, &|| false, &mut |_| {});
        assert!(record.is_ready(), "{name}: {}", record.error_message);
        let expected_waveform = record.waveform.take().expect("analyzed waveform");
        let expected_spectrogram = record.spectrogram.take().expect("analyzed spectrogram");
        let DecodedPreviews { waveform, spectrogram } = decode_previews(&path);
        assert_eq!(
            waveform, expected_waveform,
            "{name}: waveform must match same-pass analysis"
        );
        assert_eq!(
            spectrogram, expected_spectrogram,
            "{name}: spectrum must match same-pass analysis"
        );
        check_data(
            &spectrogram,
            record.sample_rate,
            record.channels as usize,
            record.length_in_samples as usize,
        );

        let directory = tempfile::tempdir().expect("creates isolated backfill cache");
        let database = directory.path().join("preview.db");
        let cache = AnalysisCache::with_path(&database);
        cache
            .store_analysis(&record)
            .expect("stores measurements without previews");
        let reset = Connection::open(&database).expect("opens artifact reset connection");
        clear_previews(&reset, &record);
        assert!(cache.get_waveform(&path).is_none());
        assert!(cache.get_spectrogram(&path).is_none());
        assert!(
            cache
                .store_waveform(&record, &waveform)
                .expect("preflight waveform publication")
        );
        assert!(
            cache
                .store_spectrogram(&record, &spectrogram)
                .expect("preflight spectrum publication")
        );
        assert_eq!(cache.get_waveform(&path).as_deref(), Some(waveform.as_ref()));
        assert_eq!(cache.get_spectrogram(&path).as_deref(), Some(spectrogram.as_ref()));
        let measurements = cache.get_analysis(&path).expect("preview writes preserve measurements");
        assert!(measurements.waveform.is_none() && measurements.spectrogram.is_none());

        group.throughput(Throughput::Elements(record.length_in_samples));
        group.bench_function(BenchmarkId::new("decode_build", name), |bencher| {
            bencher.iter(|| black_box(decode_previews(black_box(&path))));
        });
        group.bench_function(BenchmarkId::new("decode_build_store", name), |bencher| {
            bencher.iter_batched(
                || clear_previews(&reset, &record),
                |()| {
                    let DecodedPreviews { waveform, spectrogram } = decode_previews(black_box(&path));
                    let waveform_stored = cache.store_waveform(&record, &waveform).expect("publishes waveform");
                    let spectrum_stored = cache
                        .store_spectrogram(&record, &spectrogram)
                        .expect("publishes spectrum");
                    black_box((waveform, spectrogram, waveform_stored, spectrum_stored))
                },
                BatchSize::PerIteration,
            );
        });
    }
    group.finish();
}

criterion_group!(
    benches,
    bench_setup,
    bench_push,
    bench_finish,
    bench_merge,
    bench_codec,
    bench_backfill,
);
criterion_main!(benches);
