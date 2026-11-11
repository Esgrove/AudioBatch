//! Standalone native VST3 measurement and cross-host verification, without application CLI changes.
//! Decoding and binary32 sample exports are outside the benchmark's measured interval.
//! Comparison checks identical inputs and independently reproduces the test plugin's float arithmetic.

use std::ffi::OsString;
use std::fs::File;
use std::io::{BufWriter, Write};
use std::path::{Path, PathBuf};

use anyhow::{Context as _, Result, bail, ensure};
use audiobatch_core::decode::AudioFileDecoder;
use audiobatch_plugins::benchmark::{BenchmarkMode, BenchmarkOptions, BenchmarkReport, benchmark};
use serde::{Deserialize, Serialize};
use vst3_host::UiThread;

/// Invocation syntax, including the work settings shared with the C++ executable.
const USAGE: &str = "Usage: vst3_benchmark run BUNDLE INPUT OUTPUT_PREFIX host|chain INSTANCES LATENCY ITERATIONS REPEAT\n\
                    Or: vst3_benchmark compare CPP_REPORT RUST_REPORT";

/// Measurement plus paths to planar little-endian binary32 data, written after timing.
#[derive(Serialize, Deserialize)]
struct FileReport {
    #[serde(flatten)]
    report: BenchmarkReport,
    input_file: PathBuf,
    output_file: PathBuf,
}

/// Bounded planar benchmark input with its source sample rate.
struct DecodedInput {
    planes: Vec<Vec<f32>>,
    sample_rate: u32,
}

/// Loads a bounded decoded input, then repeats it in memory to amortize timer overhead.
fn decode(path: &Path, repeats: usize) -> Result<DecodedInput> {
    ensure!((1..=1000).contains(&repeats), "Repeat count must be 1..1000");
    let mut decoder = AudioFileDecoder::open(path)?;
    let info = decoder.info();
    ensure!(
        (1..=2).contains(&info.channels),
        "Benchmark input must be mono or stereo"
    );
    let rate = info.sample_rate;
    let mut planes = vec![Vec::new(); info.channels as usize];
    while let Some(chunk) = decoder.next_chunk()? {
        for (plane, chunk) in planes.iter_mut().zip(chunk) {
            ensure!(
                plane.len() + chunk.len() <= 32_000_000 / repeats,
                "Input is too long for the benchmark"
            );
            plane.extend_from_slice(chunk);
        }
    }
    Ok(DecodedInput {
        planes: planes.into_iter().map(|plane| plane.repeat(repeats)).collect(),
        sample_rate: rate,
    })
}

/// Parses a complete integer option without defaulting malformed values.
fn integer(value: &OsString) -> Result<usize> {
    value
        .to_str()
        .context("Numeric option is not UTF-8")?
        .parse()
        .context("Invalid numeric option")
}

/// Creates a full output path without requiring the output file to exist.
fn output_path(prefix: &Path, suffix: &str) -> Result<PathBuf> {
    let prefix = std::path::absolute(prefix)?;
    let mut name = prefix
        .file_name()
        .context("Output prefix needs a file name")?
        .to_os_string();
    name.push(suffix);
    Ok(prefix.with_file_name(name))
}

/// Serializes planar binary32 samples, checking flush errors before publishing the report.
fn write_samples(path: &Path, planes: &[Vec<f32>]) -> Result<()> {
    let mut writer = BufWriter::new(File::create(path)?);
    for &sample in planes.iter().flatten() {
        writer.write_all(&sample.to_le_bytes())?;
    }
    writer.flush()?;
    Ok(())
}

/// Runs a complete measurement while keeping file operations outside the timed region.
fn run(arguments: &[OsString]) -> Result<()> {
    let [bundle, input, prefix, mode, instances, latency, iterations, repeats] = arguments else {
        bail!(USAGE);
    };
    let mode = match mode.to_str() {
        Some("host") => BenchmarkMode::Host,
        Some("chain") => BenchmarkMode::Chain,
        _ => bail!("Benchmark mode must be host or chain"),
    };
    let options = BenchmarkOptions {
        mode,
        instances: integer(instances)?,
        latency: u32::try_from(integer(latency)?)?,
        iterations: integer(iterations)?,
    };
    let DecodedInput {
        planes,
        sample_rate: rate,
    } = decode(Path::new(input), integer(repeats)?)?;
    let dispatcher = UiThread::spawn_dedicated()?;
    let output = benchmark(&dispatcher, Path::new(bundle), &planes, rate, options)?;
    let input_file = output_path(Path::new(prefix), ".input.f32")?;
    let output_file = output_path(Path::new(prefix), ".output.f32")?;
    write_samples(&input_file, &planes)?;
    write_samples(&output_file, &output.samples)?;
    let report = FileReport {
        report: output.report,
        input_file,
        output_file,
    };
    println!("{}", serde_json::to_string_pretty(&report)?);
    Ok(())
}

/// Reads a checked planar sample export without accepting truncated or oversized data.
fn samples(path: &Path, report: &BenchmarkReport) -> Result<Vec<Vec<f32>>> {
    ensure!(
        (1..=2).contains(&report.channels) && (1..=32_000_000).contains(&report.input_frames),
        "Invalid exported sample geometry"
    );
    let bytes = std::fs::read(path)?;
    ensure!(
        bytes.len() == report.channels * report.input_frames * 4,
        "Sample export has the wrong length"
    );
    let planes: Vec<Vec<f32>> = bytes
        .as_chunks::<4>()
        .0
        .chunks_exact(report.input_frames)
        .map(|plane| plane.iter().map(|bytes| f32::from_le_bytes(*bytes)).collect())
        .collect();
    ensure!(
        planes.iter().flatten().all(|sample| sample.is_finite()),
        "Sample export contains nonfinite values"
    );
    Ok(planes)
}

/// Repeats each nonfused binary32 operation of `AudioBatch` Test MS, independently of either host.
fn expected(input: &[Vec<f32>], report: &BenchmarkReport) -> Vec<Vec<f32>> {
    let mut output = input.to_vec();
    for _ in 0..report.instances {
        match output.as_mut_slice() {
            [mono] => {
                for sample in mono {
                    *sample *= report.gain;
                }
            }
            [left, right] => {
                for (left, right) in left.iter_mut().zip(right) {
                    let sum = *left + *right;
                    let difference = *left - *right;
                    let mid = sum * 0.5;
                    let half_side = difference * 0.5;
                    let side = half_side * report.width;
                    *left = (mid + side) * report.gain;
                    *right = (mid - side) * report.gain;
                }
            }
            _ => unreachable!("exported sample geometry is validated"),
        }
    }
    output
}

/// Matches the production processed-file quantizer, which floors and saturates without dither.
fn pcm_step(sample: f32) -> i32 {
    (sample * 32_768.0).clamp(-32_768.0, 32_767.0).floor() as i32
}

/// Checks every independent expected sample in both floating-point and final 16-bit PCM form.
fn verify(expected: &[Vec<f32>], actual: &[Vec<f32>]) -> Result<f64> {
    ensure!(
        expected.len() == actual.len()
            && expected
                .iter()
                .zip(actual)
                .all(|(expected, actual)| expected.len() == actual.len()),
        "Output sample geometry differs"
    );
    let mut peak = 0.0_f64;
    let mut differing_steps = 0_u64;
    for (&expected, &actual) in expected.iter().flatten().zip(actual.iter().flatten()) {
        ensure!(
            expected.is_finite() && actual.is_finite(),
            "Plugin output or reference is not finite"
        );
        peak = peak.max((f64::from(actual) - f64::from(expected)).abs());
        differing_steps += u64::from(pcm_step(actual) != pcm_step(expected));
    }
    ensure!(
        differing_steps == 0,
        "{differing_steps} PCM samples differ from independent expectations"
    );
    ensure!(
        peak <= 0.0,
        "Float output differs from independent arithmetic, residual {peak}"
    );
    Ok(peak)
}

/// Summarizes individual elapsed times without rejecting outliers or imposing timing thresholds.
fn statistics(report: &BenchmarkReport) -> Result<serde_json::Value> {
    ensure!(
        report.block_frames > 0
            && report.rendered_frames > 0
            && report.rendered_frames.is_multiple_of(report.block_frames),
        "Invalid rendered block geometry"
    );
    let times = &report.seconds;
    ensure!(
        (1..=1000).contains(&times.len()) && times.iter().all(|time| time.is_finite() && *time > 0.0),
        "Invalid timing samples"
    );
    let mean = times.iter().sum::<f64>() / times.len() as f64;
    let variance = if times.len() > 1 {
        times.iter().map(|time| (time - mean).powi(2)).sum::<f64>() / (times.len() - 1) as f64
    } else {
        0.0
    };
    let mut ordered = times.clone();
    ordered.sort_by(f64::total_cmp);
    let middle = ordered.len() / 2;
    let median = if ordered.len().is_multiple_of(2) {
        f64::midpoint(
            *ordered.get(middle - 1).expect("nonempty even sample count"),
            *ordered.get(middle).expect("bounded median"),
        )
    } else {
        *ordered.get(middle).expect("bounded median")
    };
    let frames_per_second = report.rendered_frames as f64 / mean;
    let nanoseconds_per_block = mean * 1e9 / (report.rendered_frames / report.block_frames) as f64;
    ensure!(
        mean.is_finite() && variance.is_finite() && frames_per_second.is_finite() && nanoseconds_per_block.is_finite(),
        "Timing statistics overflow"
    );
    Ok(serde_json::json!({
        "mean_seconds": mean, "stddev_seconds": variance.sqrt(), "median_seconds": median,
        "frames_per_second": frames_per_second,
        "nanoseconds_per_block": nanoseconds_per_block,
        "runs": times.len()
    }))
}

/// Rejects incomparable workloads and checks every sample before reporting a host timing ratio.
fn compare(cpp_path: &Path, rust_path: &Path) -> Result<()> {
    let cpp: FileReport = serde_json::from_reader(File::open(cpp_path)?)?;
    let rust: FileReport = serde_json::from_reader(File::open(rust_path)?)?;
    let first = &cpp.report;
    let second = &rust.report;
    ensure!(
        (1..=8).contains(&first.instances)
            && (1..=2).contains(&first.channels)
            && (1..=32_000_000).contains(&first.input_frames)
            && (16..=2_822_400).contains(&first.sample_rate)
            && first.gain.is_finite()
            && first.width.is_finite()
            && first.latency_frames <= first.instances * 1024
            && first.rendered_frames == (first.input_frames + first.latency_frames).div_ceil(1024) * 1024
            && (first.mode != BenchmarkMode::Host || first.instances == 1),
        "Invalid benchmark work settings"
    );
    ensure!(
        first.host == "cpp" && second.host == "rust",
        "Expected a C++ and a Rust report"
    );
    ensure!(
        first.mode == second.mode
            && first.channels == second.channels
            && first.sample_rate == second.sample_rate
            && first.input_frames == second.input_frames
            && first.rendered_frames == second.rendered_frames
            && first.block_frames == 1024
            && second.block_frames == 1024
            && first.instances == second.instances
            && first.latency_frames == second.latency_frames
            && first.gain.to_bits() == second.gain.to_bits()
            && first.width.to_bits() == second.width.to_bits(),
        "Hosts measured different work geometry or settings"
    );
    let input = samples(&cpp.input_file, first)?;
    let other_input = samples(&rust.input_file, second)?;
    ensure!(
        input
            .iter()
            .flatten()
            .zip(other_input.iter().flatten())
            .all(|(a, b)| a.to_bits() == b.to_bits()),
        "Decoded inputs differ between hosts"
    );
    let reference = expected(&input, first);
    let cpp_output = samples(&cpp.output_file, first)?;
    let rust_output = samples(&rust.output_file, second)?;
    let cpp_residual = verify(&reference, &cpp_output)?;
    let rust_residual = verify(&reference, &rust_output)?;
    let cpp_stats = statistics(first)?;
    let rust_stats = statistics(second)?;
    let ratio = first.seconds.iter().sum::<f64>()
        / first.seconds.len() as f64
        / (second.seconds.iter().sum::<f64>() / second.seconds.len() as f64);
    println!(
        "{}",
        serde_json::to_string(&serde_json::json!({
            "mode": first.mode, "sample_rate": first.sample_rate, "channels": first.channels,
            "instances": first.instances, "latency_frames": first.latency_frames,
            "cpp": cpp_stats, "rust": rust_stats, "cpp_over_rust": ratio,
            "cpp_float_residual": cpp_residual, "rust_float_residual": rust_residual,
            "differing_pcm_samples": 0, "input_frames": first.input_frames
        }))?
    );
    Ok(())
}

/// Selects the standalone benchmark or verifier and surfaces all errors through the process exit status.
fn main() -> Result<()> {
    let arguments: Vec<_> = std::env::args_os().skip(1).collect();
    match arguments.as_slice() {
        [command, rest @ ..] if command == "run" => run(rest),
        [command, cpp, rust] if command == "compare" => compare(Path::new(cpp), Path::new(rust)),
        _ => bail!(USAGE),
    }
}

#[cfg(test)]
mod test_verification {
    use super::*;

    /// A bounded independent-reference workload, without loading a native plugin.
    fn report(instances: usize) -> BenchmarkReport {
        BenchmarkReport {
            host: "rust".into(),
            mode: BenchmarkMode::Chain,
            channels: 2,
            sample_rate: 48_000,
            input_frames: 1,
            rendered_frames: 1024,
            block_frames: 1024,
            instances,
            latency_frames: 0,
            gain: 0.5,
            width: 0.5,
            seconds: vec![1.0],
        }
    }

    #[test]
    fn independent_stereo_arithmetic_preserves_stage_rounding() {
        let input = [vec![0.5], vec![-0.25]];
        assert_eq!(expected(&input, &report(1)), [vec![0.15625], vec![-0.03125]]);
        assert_eq!(expected(&input, &report(2)), [vec![0.0546875], vec![0.0078125]]);
    }

    #[test]
    fn verification_rejects_truncation_nonfinite_and_differing_pcm() {
        let reference = [vec![0.5]];
        assert!(verify(&reference, &[vec![]]).is_err());
        assert!(verify(&reference, &[vec![f32::NAN]]).is_err());
        assert!(verify(&[vec![f32::NAN]], &[vec![0.0]]).is_err());
        assert!(verify(&reference, &[vec![0.51]]).is_err());
        assert!(verify(&reference, &reference).is_ok());
    }

    #[test]
    fn timing_validation_rejects_invalid_samples() {
        for seconds in [vec![], vec![0.0], vec![f64::NAN], vec![f64::MAX; 2]] {
            let mut report = report(1);
            report.seconds = seconds;
            assert!(statistics(&report).is_err());
        }
        for (block_frames, rendered_frames) in [(0, 1024), (1024, 0), (1024, 1025)] {
            let mut report = report(1);
            report.block_frames = block_frames;
            report.rendered_frames = rendered_frames;
            assert!(statistics(&report).is_err());
        }
    }
}
