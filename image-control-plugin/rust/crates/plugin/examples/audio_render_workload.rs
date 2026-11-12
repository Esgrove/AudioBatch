//! Precomputes shared GPU input from moving broadband audio through the production analyzer and particle builder.

use std::fmt::Write as _;
use std::path::PathBuf;

use anyhow::{Context as _, Result, ensure};
use image_control_dsp::{ColorMode, DisplaySettings, FrequencyScale, ParticleBuilder, SpatialAnalyzer};
use serde_json::json;
use sha2::{Digest as _, Sha256};

#[path = "../../../benchmarks/workload.rs"]
#[expect(
    dead_code,
    reason = "GPU workload export shares audio generation, not saved DSP scenarios"
)]
mod workload;

/// Source and render cadence are independent of benchmark execution speed.
const SAMPLE_RATE: usize = 48_000;
/// Eight seconds fit the shared format's 240-frame bound.
const RENDER_RATE: usize = 30;
/// Retains the shared binary format's bounded storage contract.
const MAXIMUM_BYTES: usize = 256 * 1024 * 1024;

/// Prepared frames retain real counts separately from transparent fixed-stride padding.
struct AudioWorkload {
    /// Validated production instance attributes in chronological render-frame order.
    frames: Vec<Vec<[f32; 8]>>,
    /// Exact precomputed stereo sample identity, including analyzer warmup.
    audio_fnv1a64: String,
}

/// Produces the same exact byte identity used by both native benchmark readers.
fn fnv1a64(bytes: impl Iterator<Item = u8>) -> String {
    let hash = bytes.fold(0xcbf2_9ce4_8422_2325_u64, |hash, byte| {
        (hash ^ u64::from(byte)).wrapping_mul(0x0000_0100_0000_01b3)
    });
    format!("{hash:016x}")
}

/// Formats standard SHA-256 without relying on digest output formatting traits.
fn sha256(bytes: &[u8]) -> String {
    Sha256::digest(bytes)
        .iter()
        .fold(String::with_capacity(64), |mut output, byte| {
            write!(&mut output, "{byte:02x}").expect("writing to String");
            output
        })
}

/// Warms all FFT tiers for one second, then preserves analysis and adaptive display history across frames.
fn generate(frame_count: usize) -> Result<AudioWorkload> {
    let frames_per_render = SAMPLE_RATE / RENDER_RATE;
    let audio =
        workload::SignalKind::MovingNoise.frames(SAMPLE_RATE as f64, 2, SAMPLE_RATE + frame_count * frames_per_render);
    let audio_fnv1a64 = fnv1a64(audio.iter().flatten().flat_map(|sample| sample.to_le_bytes()));
    let mut analyzer = SpatialAnalyzer::new(SAMPLE_RATE as f32).context("production analyzer")?;
    let mut builder = ParticleBuilder::new();
    builder.set_settings(
        DisplaySettings::new(ColorMode::Frequency, FrequencyScale::Logarithmic, 1.0, 0.6)
            .context("production display settings")?,
    );
    let (warmup, source) = audio.split_at_checked(SAMPLE_RATE).context("complete audio warmup")?;
    analyzer.push_frames(warmup, |points, info| builder.add_frame(points, info))?;
    builder.clear();
    let mut frames = Vec::with_capacity(frame_count);
    for chunk in source.chunks_exact(frames_per_render) {
        analyzer.push_frames(chunk, |points, info| builder.add_frame(points, info))?;
        let frame = builder
            .particles()
            .iter()
            .filter_map(|particle| {
                let [horizontal, vertical] = particle.position();
                let [red, green, blue] = particle.color();
                let attributes = [
                    horizontal,
                    vertical,
                    particle.spread(),
                    red,
                    green,
                    blue,
                    particle.size(),
                    particle.emphasis(),
                ];
                (attributes
                    .iter()
                    .all(|value| value.is_finite() && (0.0..=1.0).contains(value))
                    && particle.size() > 0.0)
                    .then_some(attributes)
            })
            .collect::<Vec<_>>();
        ensure!(
            frame.len() == builder.particles().len(),
            "production particles exceed the shared input domain"
        );
        frames.push(frame);
        builder.clear();
    }
    ensure!(
        frames.iter().all(|frame| !frame.is_empty()),
        "broadband analysis produced an empty frame"
    );
    Ok(AudioWorkload { frames, audio_fnv1a64 })
}

/// Pads only zero-energy records; real production observations are neither sampled nor duplicated.
fn encode(workload: &AudioWorkload) -> Result<Vec<u8>> {
    let count = workload.frames.iter().map(Vec::len).max().context("render frames")?;
    ensure!(count <= 65_536, "production frame exceeds the shared particle bound");
    let size = 24 + workload.frames.len() * count * 32;
    ensure!(size <= MAXIMUM_BYTES, "audio workload exceeds shared storage bound");
    let mut bytes = Vec::with_capacity(size);
    bytes.extend_from_slice(b"ICRB0001");
    bytes.extend_from_slice(&u32::try_from(workload.frames.len())?.to_le_bytes());
    bytes.extend_from_slice(&u32::try_from(count)?.to_le_bytes());
    bytes.extend_from_slice(&(1.0 / RENDER_RATE as f32).to_le_bytes());
    bytes.extend_from_slice(&1.0_f32.to_le_bytes());
    for frame in &workload.frames {
        for attributes in frame
            .iter()
            .copied()
            .chain(std::iter::repeat([0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.5, 0.0]))
            .take(count)
        {
            for value in attributes {
                bytes.extend_from_slice(&value.to_le_bytes());
            }
        }
    }
    Ok(bytes)
}

/// Parses before generation and writes a new exact workload plus the shared self-hashed JSON sidecar.
fn main() -> Result<()> {
    let arguments: Vec<_> = std::env::args_os().skip(1).collect();
    if arguments.iter().any(|value| value == "-h" || value == "--help") {
        println!(
            "Usage: audio_render_workload -o|--output PATH [-f|--frames 1..240]\n\
                  Seeded multiband moving noise, 48 kHz, one-second analyzer warmup, 30 Hz render frames.\n\
                  Default: 240 frames (eight seconds). Writes ICRB0001 and a .json sidecar."
        );
        return Ok(());
    }
    let mut output = None;
    let mut frame_count = 240;
    let mut arguments = arguments.into_iter();
    while let Some(option) = arguments.next() {
        let value = arguments.next().context("option requires a value")?;
        match option.to_str() {
            Some("-o" | "--output") => output = Some(PathBuf::from(value)),
            Some("-f" | "--frames") => frame_count = value.to_str().context("UTF-8 frame count")?.parse()?,
            _ => anyhow::bail!("unknown option {}", option.to_string_lossy()),
        }
    }
    ensure!((1..=240).contains(&frame_count), "frames must be 1..240");
    let output = output.context("--output is required")?;
    let manifest_path = PathBuf::from(format!("{}.json", output.display()));
    ensure!(
        !output.exists() && !manifest_path.exists(),
        "workload or manifest already exists"
    );
    let workload = generate(frame_count)?;
    let bytes = encode(&workload)?;
    let counts: Vec<_> = workload.frames.iter().map(Vec::len).collect();
    let mut manifest = json!({
        "schema_version": 1,
        "generator": "imagecontrol-moving-noise-production-analysis-v1",
        "seed": 0x1c20_2601_u32,
        "header": {"frame_count": frame_count, "particles_per_frame": counts.iter().max(),
                   "step_seconds": f64::from(1.0 / RENDER_RATE as f32), "persistence_seconds": 1.0},
        "layout": {"magic": "ICRB0001", "header_bytes": 24, "particle_bytes": 32},
        "bytes": bytes.len(), "sha256": sha256(&bytes), "fnv1a64": fnv1a64(bytes.iter().copied()),
        "audio": {"kind": "moving-noise", "sample_rate": SAMPLE_RATE, "channels": 2,
                  "warmup_frames": SAMPLE_RATE, "frames": SAMPLE_RATE + frame_count * SAMPLE_RATE / RENDER_RATE,
                  "hash_algorithm": "fnv1a64-f32le-frame-major", "hash": workload.audio_fnv1a64},
        "active_particles_per_frame": counts,
        "padding": "zero-energy records to the maximum active count; no resampling or truncation"
    });
    let manifest_hash = json!(sha256(&serde_json::to_vec(&manifest)?));
    manifest
        .as_object_mut()
        .context("manifest object")?
        .insert("manifest_sha256".into(), manifest_hash);
    std::fs::write(&output, bytes)?;
    std::fs::write(manifest_path, format!("{}\n", serde_json::to_string_pretty(&manifest)?))?;
    println!("audio-derived GPU workload: {}", output.display());
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn production_audio_particles_cover_the_spectrum_and_move_between_frames() {
        let first = generate(12).expect("audio workload");
        assert_eq!(
            encode(&first).expect("encoding"),
            encode(&generate(12).expect("repeat")).expect("encoding")
        );
        let particles: Vec<_> = first.frames.iter().flatten().collect();
        assert!(particles.iter().any(|values| values[0] < 0.1));
        assert!(particles.iter().any(|values| values[0] > 0.9));
        assert!(particles.iter().any(|values| values[1] < 0.35));
        assert!(particles.iter().any(|values| values[1] > 0.65));
        let means: Vec<f32> = (0..6)
            .map(|band| {
                let region = band as f32 / 6.0..(band + 1) as f32 / 6.0;
                let positions: Vec<_> = particles
                    .iter()
                    .filter(|values| region.contains(&values[0]))
                    .map(|values| values[1])
                    .collect();
                assert!(!positions.is_empty(), "each frequency region must remain visible");
                positions.iter().sum::<f32>() / positions.len() as f32
            })
            .collect();
        let minimum = means.iter().copied().fold(f32::INFINITY, f32::min);
        let maximum = means.iter().copied().fold(f32::NEG_INFINITY, f32::max);
        assert!(
            maximum - minimum > 0.2,
            "frequency regions must have distinct stereo placement"
        );
        assert_ne!(first.frames.first(), first.frames.last());
    }
}
