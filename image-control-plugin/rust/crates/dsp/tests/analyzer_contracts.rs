//! Independent DFT, calibration, phase, reassignment, and audio-time contracts for production analysis.

use image_control_dsp::{SpatialAnalyzer, SpectralFrameInfo, SpectralPoint, StereoPositionCurve};
use realfft::num_complex::Complex64;

const LAYOUTS: [ExpectedTier; 6] = [
    ExpectedTier {
        window_length: 32_768,
        hop: 2048,
        min_frequency: 20.0,
        max_frequency: 80.0,
    },
    ExpectedTier {
        window_length: 16_384,
        hop: 1024,
        min_frequency: 80.0,
        max_frequency: 160.0,
    },
    ExpectedTier {
        window_length: 8192,
        hop: 512,
        min_frequency: 160.0,
        max_frequency: 320.0,
    },
    ExpectedTier {
        window_length: 4096,
        hop: 512,
        min_frequency: 320.0,
        max_frequency: 640.0,
    },
    ExpectedTier {
        window_length: 2048,
        hop: 512,
        min_frequency: 640.0,
        max_frequency: 4000.0,
    },
    ExpectedTier {
        window_length: 1024,
        hop: 512,
        min_frequency: 4000.0,
        max_frequency: 20_000.0,
    },
];

/// Independent expected window geometry and reported range, without sharing production configuration.
struct ExpectedTier {
    window_length: usize,
    hop: usize,
    min_frequency: f64,
    max_frequency: f64,
}

/// Samples an analytical stereo sinusoid in double precision before the f32 audio boundary.
fn sine(length: usize, rate: f64, frequency: f64, left: f64, right: f64, right_phase: f64) -> Vec<[f32; 2]> {
    (0..length)
        .map(|index| {
            let phase = std::f64::consts::TAU * frequency * index as f64 / rate;
            [
                (phase.sin() * left) as f32,
                ((phase + right_phase).sin() * right) as f32,
            ]
        })
        .collect()
}

/// Computes an explicit Hann or Hann-derivative DFT with leading zero history, without using an FFT.
fn direct_dft(
    input: &[[f32; 2]],
    endpoint: usize,
    window: usize,
    frequency: f64,
    rate: f64,
    derivative: bool,
) -> [Complex64; 2] {
    let mut result = [Complex64::default(); 2];
    for sample in 0..window {
        let Some(source) = (endpoint + sample)
            .checked_sub(window)
            .and_then(|source| input.get(source))
        else {
            continue;
        };
        let window_phase = std::f64::consts::TAU * sample as f64 / window as f64;
        let weight = if derivative {
            std::f64::consts::PI / window as f64 * window_phase.sin()
        } else {
            0.5_f64.mul_add(-window_phase.cos(), 0.5)
        };
        let rotation = Complex64::from_polar(weight, -std::f64::consts::TAU * frequency * sample as f64 / rate);
        for (sum, &value) in result.iter_mut().zip(source) {
            *sum += rotation * f64::from(value);
        }
    }
    result
}

/// Collects owned observations solely in tests, so production callback slices remain ephemeral.
fn collect(input: &[[f32; 2]], chunks: usize) -> Vec<(SpectralFrameInfo, Vec<SpectralPoint>)> {
    let mut analyzer = SpatialAnalyzer::new(48_000.0).expect("supported rate");
    let mut frames = Vec::new();
    for chunk in input.chunks(chunks) {
        analyzer
            .push_frames(chunk, |points, frame| frames.push((frame, points.to_vec())))
            .expect("analysis");
    }
    frames
}

#[test]
fn exact_deadlines_start_at_first_hop_and_groups_are_chunk_independent() {
    let input = sine(8217, 48_000.0, 1117.25, 0.3, 0.7, 0.4);
    let whole = collect(&input, input.len());
    let split = collect(&input, 37);
    assert_eq!(
        whole, split,
        "every field and point must be bit-identical across chunks"
    );
    let mut count = 0;
    for endpoint in (512..=8192).step_by(512) {
        let due: Vec<_> = LAYOUTS
            .iter()
            .enumerate()
            .filter(|(_, layout)| endpoint % layout.hop == 0)
            .collect();
        for (due_index, &(tier, layout)) in due.iter().enumerate() {
            let (frame, points) = whole.get(count).expect("frame at expected endpoint");
            assert_eq!(frame.tier_index, tier);
            assert_eq!(frame.frame_seconds.to_bits(), (layout.hop as f64 / 48_000.0).to_bits());
            assert_eq!(frame.end_seconds.to_bits(), (endpoint as f64 / 48_000.0).to_bits());
            assert_eq!(frame.last_at_time, due_index + 1 == due.len());
            assert!(
                points
                    .iter()
                    .all(|point| (layout.min_frequency..=layout.max_frequency).contains(&f64::from(point.frequency())))
            );
            count += 1;
        }
    }
    assert_eq!(count, whole.len());
}

#[test]
fn all_tiers_match_independent_hann_dft_power_and_derivative_reassignment() {
    for (tier_index, layout) in LAYOUTS.iter().enumerate() {
        let window = layout.window_length;
        let frequency = (layout.min_frequency * layout.max_frequency).sqrt() + 0.317;
        let input = sine(32_768, 48_000.0, frequency, 0.7, 0.3, 0.8);
        let frames = collect(&input, input.len());
        let (_, points) = frames
            .iter()
            .rev()
            .find(|(frame, _)| frame.tier_index == tier_index)
            .expect("tier");
        let peak = points
            .iter()
            .max_by(|left, right| left.power().total_cmp(&right.power()))
            .expect("peak");
        let channels = direct_dft(
            &input,
            input.len(),
            window,
            f64::from(peak.frequency()),
            48_000.0,
            false,
        );
        let derivatives = direct_dft(&input, input.len(), window, f64::from(peak.frequency()), 48_000.0, true);
        let raw_power = channels.iter().map(Complex64::norm_sqr).sum::<f64>();
        let expected_power = raw_power * 16.0 / (window * window) as f64;
        assert!(
            (f64::from(peak.power()) - expected_power).abs() < 2.0e-6,
            "tier {tier_index}"
        );
        let shift = channels
            .into_iter()
            .zip(derivatives)
            .map(|(value, derivative)| (derivative * value.conj()).im)
            .sum::<f64>();
        let expected_frequency = f64::from(peak.frequency()) - 48_000.0 / std::f64::consts::TAU * shift / raw_power;
        assert!(
            (f64::from(peak.reassigned_frequency()) - expected_frequency).abs() < 0.004,
            "tier {tier_index}"
        );
        assert!(
            (f64::from(peak.reassigned_frequency()) - frequency).abs() < 0.02,
            "tier {tier_index}"
        );
        assert!((peak.spacing() - 48_000.0 / (window * 2) as f32).abs() < 1.0e-6);
    }
}

#[test]
fn leading_zero_history_matches_startup_dft_without_waiting_for_a_full_window() {
    let input = sine(512, 48_000.0, 4500.0, 1.0, 0.25, 0.3);
    let frames = collect(&input, 13);
    let (frame, points) = frames
        .iter()
        .find(|(frame, _)| frame.tier_index == 5)
        .expect("first hop");
    assert!((frame.end_seconds - 512.0 / 48_000.0).abs() < f64::EPSILON);
    let peak = points
        .iter()
        .max_by(|left, right| left.power().total_cmp(&right.power()))
        .expect("startup peak");
    let spectrum = direct_dft(&input, 512, 1024, f64::from(peak.frequency()), 48_000.0, false);
    let expected = spectrum.iter().map(Complex64::norm_sqr).sum::<f64>() * 16.0 / 1_048_576.0;
    assert!((f64::from(peak.power()) - expected).abs() < 1.0e-6);
}

#[test]
fn single_channel_calibration_and_stereo_coherence_have_analytical_values() {
    for (left, right, phase, expected_power, position, coherence) in [
        (1.0, 0.0, 0.0, 1.0, 0.0, 1.0),
        (0.0, 1.0, 0.0, 1.0, 1.0, 1.0),
        (1.0, 1.0, 0.0, 2.0, 0.5, 1.0),
        (1.0, 1.0, std::f64::consts::PI, 2.0, 0.5, -1.0),
        (1.0, 1.0, std::f64::consts::FRAC_PI_2, 2.0, 0.5, 0.0),
    ] {
        let input = sine(4096, 48_000.0, 750.0, left, right, phase);
        let frames = collect(&input, 257);
        let (_, points) = frames
            .iter()
            .rev()
            .find(|(frame, _)| frame.tier_index == 4)
            .expect("mids");
        let peak = points
            .iter()
            .max_by(|left, right| left.power().total_cmp(&right.power()))
            .expect("peak");
        assert!((peak.power() - expected_power).abs() < 1.0e-5);
        assert!((peak.position() - position).abs() < 1.0e-5);
        // A silent channel has no defined phase. Packed-FFT rounding may leave a tiny nonzero residual.
        if left > 0.0 && right > 0.0 {
            assert!((peak.coherence() - coherence).abs() < 1.0e-5);
        }
    }
}

#[test]
fn neighborhood_coherence_and_position_match_explicit_dft_sums_but_power_stays_local() {
    let input: Vec<_> = (0..4096)
        .map(|sample| {
            let time = f64::from(sample) / 48_000.0;
            let first = (std::f64::consts::TAU * 11_000.0 * time).sin();
            let second = (std::f64::consts::TAU * 11_080.0 * time).sin();
            [
                0.4_f64.mul_add(second, first) as f32,
                0.8_f64.mul_add(-second, 0.5 * first) as f32,
            ]
        })
        .collect();
    let frames = collect(&input, input.len());
    let (_, points) = frames
        .iter()
        .rev()
        .find(|(frame, _)| frame.tier_index == 5)
        .expect("highs");
    let point = points
        .iter()
        .min_by(|left, right| {
            (left.frequency() - 11_040.0)
                .abs()
                .total_cmp(&(right.frequency() - 11_040.0).abs())
        })
        .expect("bin");
    let spacing = 48_000.0 / 2048.0;
    let bin = (f64::from(point.frequency()) / spacing).round() as usize;
    let reach = ((bin as f64) * ((1.0_f64 / 64.0).exp2() - 1.0)).floor() as usize;
    assert!(reach >= 4, "the test must exercise multiple neighboring bins");
    let mut powers = [0.0; 2];
    let mut cross = 0.0;
    for neighbor in bin - reach..=bin + reach {
        let [left, right] = direct_dft(&input, input.len(), 1024, neighbor as f64 * spacing, 48_000.0, false);
        let [left_power, right_power] = &mut powers;
        *left_power += left.norm_sqr();
        *right_power += right.norm_sqr();
        cross += (left * right.conj()).re;
    }
    let [left, right] = powers;
    let expected_coherence = cross / (left * right).sqrt();
    assert!((f64::from(point.coherence()) - expected_coherence).abs() < 2.0e-5);
    let midpoint_argument = 0.5_f64.atanh();
    let exponent = (0.99_f64.atanh() / midpoint_argument).log(32.0_f64 / 12.0);
    let scale = 12.0 / midpoint_argument.powf(1.0 / exponent);
    let difference = 10.0 * (left / right).log10();
    let expected_position = (1.0 - (difference.abs() / scale).powf(exponent).tanh().copysign(difference)) * 0.5;
    assert!((f64::from(point.position()) - expected_position).abs() < 2.0e-6);
    let local = direct_dft(&input, input.len(), 1024, f64::from(point.frequency()), 48_000.0, false);
    let expected_power = local.iter().map(Complex64::norm_sqr).sum::<f64>() * 16.0 / 1_048_576.0;
    assert!((f64::from(point.power()) - expected_power).abs() < 2.0e-6);
}

#[test]
fn rates_scale_above_48k_and_reject_invalid_setup() {
    for (rate, scale) in [
        (8000.0, 1),
        (44_100.0, 1),
        (48_000.0, 1),
        (48_001.0, 2),
        (96_000.0, 2),
        (192_000.0, 4),
        (768_000.0, 16),
    ] {
        let mut analyzer = SpatialAnalyzer::new(rate).expect("rate boundary");
        let mut metadata = Vec::new();
        analyzer
            .push_frames(&vec![[0.0; 2]; 2048 * scale], |points, frame| {
                assert_eq!(points, []);
                metadata.push(frame);
            })
            .expect("silence");
        for (tier, layout) in LAYOUTS.iter().enumerate() {
            let first = metadata
                .iter()
                .find(|frame| frame.tier_index == tier)
                .expect("all tiers report");
            assert_eq!(
                first.end_seconds.to_bits(),
                ((layout.hop * scale) as f64 / f64::from(rate)).to_bits()
            );
        }
    }
    for rate in [0.0, 7999.0, 768_001.0, f32::NAN, f32::INFINITY] {
        assert!(SpatialAnalyzer::new(rate).is_err());
    }
}

#[test]
fn reassignment_is_bounded_for_arbitrary_content_and_nonfinite_samples_preserve_the_clock() {
    let mut state = 7_u32;
    let input: Vec<_> = (0..4096)
        .map(|index| {
            state = state.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
            let left = (state as f32 / u32::MAX as f32 - 0.5) * 0.4;
            [if index == 99 { f32::NAN } else { left }, left * 0.3]
        })
        .collect();
    let clean: Vec<_> = input
        .iter()
        .map(|&frame| frame.map(|sample| if sample.is_finite() { sample } else { 0.0 }))
        .collect();
    let actual = collect(&input, 37);
    assert_eq!(actual, collect(&clean, input.len()));
    for (_, points) in &actual {
        for point in points {
            assert!((point.reassigned_frequency() - point.frequency()).abs() <= point.spacing().mul_add(4.0, 0.001));
        }
    }
    let mut analyzer = SpatialAnalyzer::new(48_000.0).expect("rate");
    analyzer.push(&input, &mut Vec::new()).expect("warmup");
    analyzer.reset();
    let mut restarted = Vec::new();
    analyzer
        .push_frames(&clean, |points, frame| restarted.push((frame, points.to_vec())))
        .expect("restart");
    assert_eq!(restarted, collect(&clean, 512));
    assert!(StereoPositionCurve::new(12.0, 32.0).is_some());
    assert!(StereoPositionCurve::new(f64::NAN, 32.0).is_none());
    assert!(StereoPositionCurve::new(24.0, 25.0).is_none());
}
