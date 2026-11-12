//! Criterion probes of production particle packing, CPU submission, and completed offscreen GPU rendering.
//!
//! Native setup, input construction, shader compilation, and cold warmup are outside timed iterations.
//! Submission timing excludes batch-boundary completion waits but includes queue backpressure.
//! Completed timing includes those waits, with at most 32 frames in flight. Neither measures display presentation.

use std::fmt::Write as _;
use std::hint::black_box;
use std::time::Duration;

use criterion::{BenchmarkId, Criterion, Throughput, criterion_group, criterion_main};
use image_control_plugin_rs::render_benchmark::{PreparedRenderer, RenderSettings};
use sha2::{Digest as _, Sha256};

/// Same common particle ceilings as the cross-implementation renderer workload.
const PARTICLE_COUNTS: [u32; 4] = [0, 4096, 16_384, 65_536];
/// Physical target dimensions, not logical window sizes or assumptions about monitor DPI.
const TARGETS: [[u32; 2]; 2] = [[1280, 720], [2560, 1440]];
/// Integer generator and attribute layout match the versioned shared ICRB0001 workload specification.
const WORKLOAD_SEED: u32 = 0x1c20_2601;
/// Explicit simulated native-refresh step, independent of Criterion's unpaced execution speed.
const FRAME_STEP_SECONDS: f32 = 1.0 / 120.0;
const PERSISTENCE_SECONDS: f32 = 0.3;

/// Builds one immutable frame using exact powers-of-two fractions before entering any timed interval.
fn workload(particles: u32) -> Vec<u8> {
    let mut bytes = Vec::with_capacity(24 + particles as usize * 32);
    bytes.extend_from_slice(b"ICRB0001");
    bytes.extend_from_slice(&1_u32.to_le_bytes());
    bytes.extend_from_slice(&particles.to_le_bytes());
    bytes.extend_from_slice(&FRAME_STEP_SECONDS.to_le_bytes());
    bytes.extend_from_slice(&PERSISTENCE_SECONDS.to_le_bytes());
    let mut state = WORKLOAD_SEED;
    for _ in 0..particles {
        let [x, y, spread, red, green, blue, size, emphasis] = std::array::from_fn(|_| {
            state = state.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
            (state >> 8) as f32 * (1.0 / 16_777_216.0)
        });
        let attributes = [
            x,
            y,
            (1 + (spread * 16.0) as u32) as f32 / 4096.0,
            red,
            green,
            blue,
            size.mul_add(0.5, 0.5),
            emphasis,
        ];
        for value in attributes {
            bytes.extend_from_slice(&value.to_le_bytes());
        }
    }
    bytes
}

/// Isolates reusable CPU attribute packing without including a GPU upload or completion wait.
fn particle_packing(criterion: &mut Criterion) {
    image_control_plugin_rs::benchmark_build::require_optimized_native().expect("Release native benchmark build");
    let mut group = criterion.benchmark_group("spectrogram/particle_packing_cpu");
    group.throughput(Throughput::Elements(1));
    for particles in PARTICLE_COUNTS {
        group.bench_function(BenchmarkId::from_parameter(particles), |bencher| {
            let input = workload(particles);
            let mut renderer = PreparedRenderer::new(
                &input,
                RenderSettings {
                    width: 1280,
                    height: 720,
                    scale: 1.0,
                    solid: false,
                },
            )
            .expect("native spectrogram renderer initialization");
            bencher.iter(|| black_box(renderer.prepare_particles().expect("validated particle frame")));
            renderer.validate().expect("native renderer validation");
        });
    }
    group.finish();
}

/// Samples actual warmed production passes, with CPU submission and completed wall time as separate groups.
fn production_rendering(criterion: &mut Criterion) {
    image_control_plugin_rs::benchmark_build::require_optimized_native().expect("Release native benchmark build");
    for submission_only in [true, false] {
        let name = if submission_only {
            "spectrogram/submission_cpu"
        } else {
            "spectrogram/completed_gpu_work"
        };
        let mut group = criterion.benchmark_group(name);
        group.throughput(Throughput::Elements(1));
        for [width, height] in TARGETS {
            for solid in [false, true] {
                let mode = if solid { "solid" } else { "particles" };
                for particles in PARTICLE_COUNTS {
                    let id = BenchmarkId::new(format!("{width}x{height}/{mode}"), particles);
                    group.bench_function(id, |bencher| {
                        let input = workload(particles);
                        let mut renderer = PreparedRenderer::new(
                            &input,
                            RenderSettings {
                                width,
                                height,
                                scale: 1.0,
                                solid,
                            },
                        )
                        .expect("native spectrogram renderer initialization");
                        bencher.iter_custom(|frames| {
                            if submission_only {
                                renderer.measure_submission_frames(frames)
                            } else {
                                renderer.measure_completed_frames(frames)
                            }
                            .expect("bounded production rendering completion")
                        });
                        renderer.validate().expect("native renderer validation");
                    });
                }
            }
        }
        group.finish();
    }
}

/// Adds changing production-analyzed frames from an explicit ICRB export without replacing static controls.
fn audio_rendering(criterion: &mut Criterion) {
    let Some(path) = std::env::var_os("IMAGECONTROL_RENDER_INPUT") else {
        return;
    };
    image_control_plugin_rs::benchmark_build::require_optimized_native().expect("Release native benchmark build");
    let metadata = std::fs::metadata(&path).expect("explicit audio GPU workload");
    assert!(metadata.len() <= 256 * 1024 * 1024, "bounded audio workload");
    let input = std::fs::read(&path).expect("explicit audio GPU workload bytes");
    let identity = Sha256::digest(&input)
        .iter()
        .fold(String::with_capacity(64), |mut output, byte| {
            write!(&mut output, "{byte:02x}").expect("writing to String");
            output
        });
    eprintln!(
        "audio GPU input {}: SHA-256 {identity}",
        std::path::Path::new(&path).display()
    );
    for submission_only in [true, false] {
        let name = if submission_only {
            "spectrogram/audio_input_submission_cpu"
        } else {
            "spectrogram/audio_input_completed_gpu_work"
        };
        let mut group = criterion.benchmark_group(name);
        group.throughput(Throughput::Elements(1));
        for [width, height] in TARGETS {
            for solid in [false, true] {
                let mode = if solid { "solid" } else { "particles" };
                let id = BenchmarkId::new(format!("{width}x{height}/{mode}"), &identity);
                group.bench_function(id, |bencher| {
                    let mut renderer = PreparedRenderer::new(
                        &input,
                        RenderSettings {
                            width,
                            height,
                            scale: 1.0,
                            solid,
                        },
                    )
                    .expect("validated production audio particles");
                    bencher.iter_custom(|frames| {
                        if submission_only {
                            renderer.measure_submission_frames(frames)
                        } else {
                            renderer.measure_completed_frames(frames)
                        }
                        .expect("bounded production audio rendering completion")
                    });
                    renderer.validate().expect("audio renderer validation");
                });
            }
        }
        group.finish();
    }
}

criterion_group! {
    name = spectrogram;
    config = Criterion::default()
        .sample_size(20)
        .warm_up_time(Duration::from_secs(1))
        .measurement_time(Duration::from_secs(3));
    targets = particle_packing, production_rendering, audio_rendering
}
criterion_main!(spectrogram);
