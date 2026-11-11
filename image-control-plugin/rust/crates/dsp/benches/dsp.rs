//! Production Imager preparation, warm host-block processing, and worker-side spatial analysis benchmarks.
//!
//! Warm timings exclude allocation, preparation, reset, input generation, and history priming.
//! Every Criterion batch resets to deterministic controls and consumes immutable source frames, never its own output.
//! Throughput counts audio frames, not interleaved channel values. These are CPU costs, not device deadlines.

use std::hint::black_box;
use std::time::Instant;

use criterion::{BatchSize, BenchmarkId, Criterion, Throughput, criterion_group, criterion_main};
use image_control_dsp::{Imager, Parameters, SpatialAnalyzer};

#[path = "../../../benchmarks/workload.rs"]
mod workload;

use workload::{Scenario, input_frame};

/// Four production FIR geometries, including both base-rate families.
const SAMPLE_RATES: [u32; 4] = [44_100, 48_000, 96_000, 192_000];
/// One iteration covers enough host blocks to amortize timing and expose partition boundaries.
const ITERATION_FRAMES: usize = 4096;
/// Host sizes include small callbacks, an exact partition, a large block, and non-partition-aligned adaptation.
const HOST_BLOCKS: [(&str, &[usize]); 4] = [
    ("64", &[64]),
    ("256", &[256]),
    ("1024", &[1024]),
    ("irregular", &[1, 17, 257, 511]),
];

/// Publishes saved controls outside preparation so the first processed frame sees settled targets.
fn configure(imager: &mut Imager, scenario: Scenario) {
    let shared = imager.shared();
    for band in 0..4 {
        for (field, value) in scenario.band(band).into_iter().enumerate() {
            shared.set_band_parameter(band, field, value as f32);
        }
    }
    let gain = if scenario == Scenario::Gains { 0.8 } else { 1.0 };
    shared.set_parameters(Parameters::new(gain, 1.0).expect("finite benchmark controls"));
    shared.set_bypass(scenario == Scenario::Bypass);
    shared.set_delta(scenario == Scenario::Delta);
    shared.set_mono(scenario == Scenario::Mono);
    imager.set_channels(scenario.channels());
}

/// Runs realistic host boundaries with preallocated output and immutable input, including the shortened last block.
fn process_blocks(imager: &mut Imager, input: &[[f32; 2]], output: &mut [[f32; 2]], blocks: &[usize]) {
    let mut position = 0;
    for size in blocks.iter().cycle() {
        if position == input.len() {
            break;
        }
        let end = (position + size).min(input.len());
        imager.begin_block();
        for (source, destination) in input
            .get(position..end)
            .expect("bounded source block")
            .iter()
            .zip(output.get_mut(position..end).expect("bounded output block"))
        {
            *destination = imager.process_frame(*source);
        }
        position = end;
    }
    black_box(output);
}

/// Measures setup separately, including the production prepare allocation and kernel worker creation.
/// Criterion excludes destruction of the returned prepared imager from this timing.
fn preparation(criterion: &mut Criterion) {
    let mut group = criterion.benchmark_group("imager_prepare");
    for sample_rate in SAMPLE_RATES {
        group.bench_with_input(
            BenchmarkId::from_parameter(sample_rate),
            &sample_rate,
            |bencher, rate| {
                bencher.iter_batched(
                    Imager::new,
                    |mut imager| {
                        assert!(imager.prepare(black_box(*rate as f32)));
                        black_box(imager)
                    },
                    BatchSize::PerIteration,
                );
            },
        );
    }
    group.finish();
}

/// Measures only the warmed production callback path, with identical deterministic initialization per timed batch.
fn warm_imager(criterion: &mut Criterion) {
    let mut group = criterion.benchmark_group("imager_warm");
    group.throughput(Throughput::Elements(ITERATION_FRAMES as u64));
    for sample_rate in SAMPLE_RATES {
        for scenario in Scenario::ALL {
            let input: Vec<_> = (0..ITERATION_FRAMES)
                .map(|index| input_frame(index, f64::from(sample_rate), scenario.channels()))
                .collect();
            let mut output = vec![[0.0; 2]; ITERATION_FRAMES];
            let mut imager = Imager::new();
            configure(&mut imager, scenario);
            assert!(imager.prepare(sample_rate as f32));
            for (block_name, blocks) in HOST_BLOCKS {
                let id = BenchmarkId::new(format!("{}/{sample_rate}", scenario.name()), block_name);
                group.bench_function(id, |bencher| {
                    bencher.iter_custom(|iterations| {
                        imager.reset();
                        let prime_frames = imager.latency_samples() as usize * 2
                            + imager.tail_samples() as usize
                            + sample_rate as usize / 50;
                        for _ in 0..prime_frames.div_ceil(ITERATION_FRAMES) {
                            process_blocks(&mut imager, &input, &mut output, blocks);
                        }
                        let started = Instant::now();
                        for _ in 0..iterations {
                            process_blocks(&mut imager, black_box(&input), &mut output, blocks);
                        }
                        started.elapsed()
                    });
                });
            }
        }
    }
    group.finish();
}

/// Measures public worker-side six-tier FFT analysis without exposing private crossover or convolver APIs.
fn spatial_analysis(criterion: &mut Criterion) {
    let mut group = criterion.benchmark_group("analyzer_warm");
    group.throughput(Throughput::Elements(ITERATION_FRAMES as u64));
    for sample_rate in SAMPLE_RATES {
        let input: Vec<_> = (0..ITERATION_FRAMES)
            .map(|index| input_frame(index, f64::from(sample_rate), 2))
            .collect();
        let mut analyzer = SpatialAnalyzer::new(sample_rate as f32).expect("supported analyzer rate");
        group.bench_function(BenchmarkId::from_parameter(sample_rate), |bencher| {
            bencher.iter_custom(|iterations| {
                analyzer.reset();
                for _ in 0..32 {
                    analyzer
                        .push_frames(&input, |points, info| {
                            black_box((points, info));
                        })
                        .expect("preallocated analyzer warmup");
                }
                let started = Instant::now();
                for _ in 0..iterations {
                    analyzer
                        .push_frames(black_box(&input), |points, info| {
                            black_box((points, info));
                        })
                        .expect("preallocated analyzer stream");
                }
                started.elapsed()
            });
        });
    }
    group.finish();
}

criterion_group!(benches, preparation, warm_imager, spatial_analysis);
criterion_main!(benches);
