//! Benchmarks of decoding alone, opening the generated files and draining them without measuring anything.
//!
//! Subtracting these from the `analyze_file` results of the analysis benchmarks gives the measurement share.
//! FLAC decode speed depends on the content, so white noise is included as its worst case,
//! and lossy decode speed depends on the bitrate, so the formats use their common high bitrates.
//! Files that have not been generated with `fixtures/benchmark/generate.py` are skipped with a note.

use std::hint::black_box;
use std::path::Path;
use std::time::Duration;

use audiobatch_core::decode::AudioFileDecoder;
use criterion::{BenchmarkId, Criterion, Throughput, criterion_group, criterion_main};

/// Generated files, every format at 44.1 kHz, the high rates as FLAC, and noise as the FLAC worst case.
const FILES: [&str; 13] = [
    "music_stereo_44100_16.wav",
    "music_stereo_44100_24.wav",
    "music_stereo_44100_16.flac",
    "music_stereo_44100_24.flac",
    "music_stereo_44100_320k.mp3",
    "music_stereo_44100_256k.m4a",
    "music_stereo_44100_q5.ogg",
    "music_mono_44100_16.flac",
    "music_stereo_96000_24.flac",
    "music_stereo_192000_24.flac",
    "music_stereo_192000_24.wav",
    "noise_stereo_44100_16.flac",
    "noise_stereo_44100_24.flac",
];

/// Decodes the whole file and returns the number of frames, so nothing is optimized away.
fn drain(path: &Path) -> u64 {
    let mut decoder = AudioFileDecoder::open(path).expect("decoder opens the file");
    let mut frames = 0;
    while let Some(chunk) = decoder.next_chunk().expect("decodes") {
        frames += chunk.first().map_or(0, Vec::len) as u64;
    }
    frames
}

/// Benchmarks opening and draining every generated file.
fn bench_decode(criterion: &mut Criterion) {
    let mut group = criterion.benchmark_group("decode");
    group.sample_size(10);
    group.measurement_time(Duration::from_secs(5));
    for name in FILES {
        let Some(path) = test_fixtures::benchmark_file(name) else {
            println!("Skipping {name}, run uv run fixtures/benchmark/generate.py to generate it");
            continue;
        };
        group.throughput(Throughput::Elements(drain(&path)));
        group.bench_with_input(BenchmarkId::from_parameter(name), &path, |bencher, path| {
            bencher.iter(|| drain(black_box(path)));
        });
    }
    group.finish();
}

criterion_group!(benches, bench_decode);
criterion_main!(benches);
