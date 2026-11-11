//! Public streaming contracts for the production complementary FIR band processor.

use std::time::{Duration, Instant};

use image_control_dsp::{BandControls, MultibandImager};

/// Constructs validated test controls without weakening finite-value validation.
fn control(mid: f32, side: f32, width: f32, recover: f32, pan: f32, solo: bool) -> BandControls {
    BandControls::new(mid, side, width, recover, pan, solo).expect("finite band controls")
}

/// Generates deterministic bounded stereo input with signed zeros and non-identical channels.
fn signal(length: usize) -> Vec<[f32; 2]> {
    (0..length)
        .map(|sample| {
            if sample.is_multiple_of(97) {
                [-0.0, 0.0]
            } else {
                let phase = sample as f32 * 0.037;
                [phase.sin() * 0.6, (phase * 1.731).cos() * 0.4]
            }
        })
        .collect()
}

/// Renders ordinary frames through arbitrary user-selected host-block boundaries.
fn render(imager: &mut MultibandImager, input: &[[f32; 2]], blocks: &[usize]) -> Vec<[f32; 2]> {
    let mut output = Vec::with_capacity(input.len());
    let mut remaining = input;
    for &block in blocks.iter().cycle() {
        if remaining.is_empty() {
            break;
        }
        let length = remaining.len().min(block);
        let (current, rest) = remaining.split_at(length);
        output.extend(current.iter().map(|&frame| imager.process_frame(frame)));
        remaining = rest;
    }
    output
}

#[test]
fn neutral_preserves_delayed_bits_and_rate_geometry() {
    for (rate, half_length) in [
        (44_100.0, 4096),
        (48_000.0, 4096),
        (88_200.0, 8192),
        (192_000.0, 16_384),
    ] {
        let mut imager = MultibandImager::new(rate).expect("supported rate");
        assert_eq!(imager.latency_samples(), half_length + 256);
        assert_eq!(imager.tail_samples(), half_length);
        let input = signal(imager.latency_samples() + 1000);
        let output = render(&mut imager, &input, &[1, 17, 1023, 256]);
        for (index, actual) in output.into_iter().enumerate() {
            let expected = index
                .checked_sub(imager.latency_samples())
                .and_then(|source| input.get(source))
                .copied()
                .unwrap_or_default();
            assert_eq!(actual.map(f32::to_bits), expected.map(f32::to_bits));
        }
    }
}

#[test]
fn unity_output_width_trims_panned_channels_without_an_extra_mid_side_roundtrip() {
    let mut ordinary = MultibandImager::new(48_000.0).expect("rate");
    let mut trimmed = MultibandImager::new(48_000.0).expect("rate");
    let pan = 1.0 - 2.0_f32.powi(-24);
    for imager in [&mut ordinary, &mut trimmed] {
        imager.set_controls([control(1.0, 1.0, 1.0, 0.0, pan, false); 4]);
    }
    assert!(trimmed.set_output_controls(0.5, 1.0, false));
    ordinary.reset();
    trimmed.reset();
    let mut quiet_power = 0.0_f64;
    for sample in 0..9000 {
        let source = (sample as f32 * 0.07).sin() * 0.5;
        let expected = ordinary.process_frame([source; 2]).map(|value| (value * 0.5).to_bits());
        let actual = trimmed.process_frame([source; 2]);
        assert_eq!(actual.map(f32::to_bits), expected, "sample {sample}");
        quiet_power += f64::from(actual[0]).powi(2);
    }
    assert!(
        quiet_power > 1.0e-15,
        "the nearly fully panned quiet channel must remain present"
    );
}

#[test]
fn output_controls_validate_transactionally_and_reset_settles_partition_targets() {
    let mut imager = MultibandImager::new(48_000.0).expect("rate");
    assert!(imager.set_output_controls(0.5, 1.0, true));
    for (gain, width) in [(f32::NAN, 1.0), (0.25, f32::INFINITY), (17.0, 1.0), (0.25, 3.0)] {
        assert!(!imager.set_output_controls(gain, width, false));
    }
    imager.reset();
    for sample in 0..imager.latency_samples() + imager.tail_samples() {
        let output = imager.process_frame([0.75, -0.25]);
        let reference = imager.dry_reference();
        for (actual, dry) in output.into_iter().zip(reference) {
            assert!(dry.mul_add(0.5, actual).abs() < 1.0e-6, "sample {sample}");
        }
    }
}

#[test]
fn controls_and_crossovers_reject_non_finite_values() {
    for bad in [f32::NAN, f32::INFINITY, f32::NEG_INFINITY] {
        assert!(BandControls::new(bad, 1.0, 1.0, 0.0, 0.0, false).is_none());
        assert!(BandControls::new(1.0, bad, 1.0, 0.0, 0.0, false).is_none());
        assert!(BandControls::new(1.0, 1.0, bad, 0.0, 0.0, false).is_none());
        assert!(BandControls::new(1.0, 1.0, 1.0, bad, 0.0, false).is_none());
        assert!(BandControls::new(1.0, 1.0, 1.0, 0.0, bad, false).is_none());
        assert!(MultibandImager::new(bad).is_err());
    }
    assert!(BandControls::new(-0.1, 1.0, 1.0, 0.0, 0.0, false).is_none());
    assert!(BandControls::new(1.0, 17.0, 1.0, 0.0, 0.0, false).is_none());
    assert!(BandControls::new(1.0, 1.0, 2.1, 0.0, 0.0, false).is_none());
    assert!(BandControls::new(1.0, 1.0, 1.0, -0.1, 0.0, false).is_none());
    let mut imager = MultibandImager::new(48_000.0).expect("rate");
    assert!(!imager.set_crossovers([100.0, f32::NAN, 5000.0]));
    assert_eq!(imager.active_crossovers(), [100.0, 500.0, 5000.0]);
    assert!(!imager.set_channels(3));
}

#[test]
fn fullband_source_width_pan_and_solo_laws_are_independent() {
    let source = [0.75_f32, -0.25_f32];
    for (settings, expected) in [
        (control(2.0, 0.5, 1.0, 0.0, 0.0, false), [0.75, 0.25]),
        (control(1.0, 1.0, 0.0, 0.0, 0.0, false), [0.25, 0.25]),
        (control(1.0, 1.0, 2.0, 0.0, 0.0, false), [1.25, -0.75]),
        (control(1.0, 1.0, 1.0, 0.0, 1.0, false), [0.0, 0.5]),
        (control(1.0, 1.0, 1.0, 0.0, -1.0, false), [0.5, 0.0]),
        (control(1.0, 1.0, 1.0, 1.0, 0.0, true), source),
    ] {
        let mut imager = MultibandImager::new(48_000.0).expect("rate");
        imager.set_controls([settings; 4]);
        imager.reset();
        let mut actual = [0.0; 2];
        for _ in 0..=imager.latency_samples() {
            actual = imager.process_frame(source);
        }
        for (actual, expected) in actual.into_iter().zip(expected) {
            assert!(
                (actual - expected).abs() < 2.0e-6,
                "actual={actual}, expected={expected}"
            );
        }
    }
}

#[test]
fn mono_ignores_stereo_only_controls_and_reset_settles_targets() {
    let mut imager = MultibandImager::new(48_000.0).expect("rate");
    assert!(imager.set_channels(1));
    imager.set_controls([control(0.5, 0.0, 0.0, 1.0, 1.0, false); 4]);
    imager.reset();
    let input = signal(imager.latency_samples() + 700);
    for (index, &[sample, _]) in input.iter().enumerate() {
        let actual = imager.process_mono(sample);
        let expected = index
            .checked_sub(imager.latency_samples())
            .and_then(|source| input.get(source))
            .map_or(0.0, |&[value, _]| value * 0.5);
        assert!((actual - expected).abs() < 2.0e-6);
    }
    imager.reset();
    for _ in 0..imager.latency_samples() {
        assert!(imager.process_mono(0.0).abs() < f32::EPSILON);
    }
}

#[test]
fn fullband_recovery_matches_independent_cotangent_impulse_and_has_finite_tail() {
    let mut imager = MultibandImager::new(48_000.0).expect("rate");
    imager.set_controls([control(0.0, 1.0, 0.0, 1.0, 0.0, false); 4]);
    imager.reset();
    let half_length = imager.tail_samples();
    let fft_size = half_length * 4;
    let support_end = 256 + half_length * 2;
    for sample in 0..support_end + 768 {
        let actual = imager.process_frame(if sample == 0 { [1.0, -1.0] } else { [0.0; 2] });
        let expected = sample
            .checked_sub(256)
            .filter(|&tap| tap <= half_length * 2)
            .map_or(0.0, |tap| {
                let offset = tap as f64 - half_length as f64;
                if tap % 2 == 0 {
                    0.0
                } else {
                    let phase = std::f64::consts::TAU * tap as f64 / (half_length * 2) as f64;
                    let window = (-0.488_29_f64).mul_add(phase.cos(), 0.358_75);
                    let window = 0.141_28_f64.mul_add((phase * 2.0).cos(), window);
                    let window = (-0.011_68_f64).mul_add((phase * 3.0).cos(), window);
                    (2.0 / fft_size as f64) / (std::f64::consts::PI * offset / fft_size as f64).tan() * window
                }
            });
        for channel in actual {
            assert!(
                (f64::from(channel) - expected).abs() < 2.0e-7,
                "sample {sample}: {channel} vs {expected}"
            );
        }
    }
}

#[test]
fn individual_solos_isolate_the_four_expected_frequency_regions() {
    for (selected, frequency) in [40.0_f32, 250.0, 2000.0, 12_000.0].into_iter().enumerate() {
        for solo_band in 0..4 {
            let mut imager = MultibandImager::new(48_000.0).expect("rate");
            imager.set_controls(std::array::from_fn(|band| {
                control(1.0, 1.0, 1.0, 0.0, 0.0, band == solo_band)
            }));
            imager.reset();
            let begin = imager.latency_samples() + imager.tail_samples();
            let end = begin + 4800;
            let mut power = 0.0_f64;
            for sample in 0..end {
                let sine = (std::f32::consts::TAU * frequency * sample as f32 / 48_000.0).sin();
                let [output, _] = imager.process_frame([sine, sine]);
                if sample >= begin {
                    power += f64::from(output).powi(2);
                }
            }
            let power = power / 4800.0;
            if selected == solo_band {
                assert!((power - 0.5).abs() < 0.002, "band {selected}: {power}");
            } else {
                assert!(power < 1.0e-6, "frequency {frequency} in band {solo_band}: {power}");
            }
        }
    }
}

#[test]
fn offline_handoffs_and_automation_do_not_depend_on_host_block_sizes() {
    let input = signal(17_000);
    let mut first = MultibandImager::new(48_000.0).expect("rate");
    let mut second = MultibandImager::new(48_000.0).expect("rate");
    for imager in [&mut first, &mut second] {
        imager.set_offline(true).expect("offline");
        imager.set_controls(std::array::from_fn(|band| {
            control(
                (band as f32).mul_add(0.15, 0.4),
                (band as f32).mul_add(0.2, 0.8),
                0.4,
                (band as f32).mul_add(0.15, 0.3),
                -0.3,
                false,
            )
        }));
        imager.reset();
    }
    for (segment, crossovers) in input.chunks(3400).zip([
        [70.0, 700.0, 7000.0],
        [200.0, 900.0, 4000.0],
        [20_000.0, 20.0, 20.0],
        [20.0, 200.0, 2000.0],
        [100.0, 500.0, 5000.0],
    ]) {
        assert!(first.set_crossovers(crossovers));
        assert!(second.set_crossovers(crossovers));
        let whole = render(&mut first, segment, &[segment.len()]);
        let split = render(&mut second, segment, &[1, 17, 257, 63]);
        for (first, second) in whole.into_iter().zip(split) {
            assert_eq!(first.map(f32::to_bits), second.map(f32::to_bits));
        }
    }
}

#[test]
fn recovery_activation_uses_warm_raw_side_history() {
    let input = signal(17_000);
    let mut always = MultibandImager::new(48_000.0).expect("rate");
    let mut activated = MultibandImager::new(48_000.0).expect("rate");
    let recovering = [control(0.0, 0.6, 0.25, 0.7, 0.0, false); 4];
    always.set_controls(recovering);
    always.reset();
    for (sample, frame) in input.into_iter().enumerate() {
        if sample == 10_000 {
            activated.set_controls(recovering);
        }
        let reference = always.process_frame(frame);
        let actual = activated.process_frame(frame);
        if sample >= 11_600 {
            for (actual, reference) in actual.into_iter().zip(reference) {
                assert!((actual - reference).abs() < 2.0e-6);
            }
        }
    }
}

#[test]
fn twenty_millisecond_band_ramps_land_exactly_without_block_restarts() {
    let mut whole = MultibandImager::new(48_000.0).expect("rate");
    let mut split = MultibandImager::new(48_000.0).expect("rate");
    let frame = [0.5_f32, -0.25_f32];
    let warmup = 12_032;
    for _ in 0..warmup {
        let _ = whole.process_frame(frame);
        let _ = split.process_frame(frame);
    }
    let muted = [control(0.0, 0.0, 1.0, 0.0, 0.0, false); 4];
    whole.set_controls(muted);
    split.set_controls(muted);
    for sample in 0_usize..1600 {
        if sample % 17 == 0 {
            split.set_controls(muted);
        }
        let actual = whole.process_frame(frame);
        let segmented = split.process_frame(frame);
        assert_eq!(actual.map(f32::to_bits), segmented.map(f32::to_bits));
        let ramp_samples = sample.saturating_sub(255).min(960);
        let gain = 1.0 - ramp_samples as f32 / 960.0;
        for (actual, source) in actual.into_iter().zip(frame) {
            assert!(source.mul_add(-gain, actual).abs() < 1.0e-5, "ramp sample {sample}");
            if sample >= 1215 {
                assert!(actual.abs() < f32::EPSILON, "the endpoint must land exactly at zero");
            }
        }
    }
}

#[test]
fn band_ramps_floor_fractional_sample_counts_and_settle_subsample_durations() {
    for rate in [1.0_f32, 11_025.0, 48_001.0] {
        let mut imager = MultibandImager::new(rate).expect("rate");
        let frame = [0.5, -0.25];
        let warmup = (imager.latency_samples() + imager.tail_samples()).div_ceil(256) * 256;
        for _ in 0..warmup {
            let _ = imager.process_frame(frame);
        }
        imager.set_controls([control(0.0, 0.0, 1.0, 0.0, 0.0, false); 4]);
        let ramp = (f64::from(rate) * 0.02).floor() as usize;
        let mut gain = 1.0_f32;
        for sample in 0..256 + ramp + 256 {
            if sample >= 256 {
                let advanced = sample - 256 + 1;
                gain = if advanced >= ramp {
                    0.0
                } else {
                    gain - 1.0 / ramp as f32
                };
            }
            for (actual, source) in imager.process_frame(frame).into_iter().zip(frame) {
                assert!(
                    source.mul_add(-gain, actual).abs() < 1.0e-6,
                    "rate={rate}, sample={sample}"
                );
                if sample >= 256 && sample + 1 >= 256 + ramp {
                    assert_eq!(actual.to_bits() & 0x7fff_ffff, 0, "exact muted endpoint");
                }
            }
        }
    }
}

#[test]
fn crossover_replacement_is_one_coherent_partition_fade_of_warm_firs() {
    let mut old = MultibandImager::new(48_000.0).expect("rate");
    let mut new = MultibandImager::new(48_000.0).expect("rate");
    let mut changing = MultibandImager::new(48_000.0).expect("rate");
    let settings = std::array::from_fn(|band| control((band as f32).mul_add(0.2, 0.3), 0.7, 0.3, 0.8, 0.0, band == 1));
    for imager in [&mut old, &mut new, &mut changing] {
        imager.set_offline(true).expect("offline");
        imager.set_controls(settings);
        imager.reset();
    }
    let crossovers = [220.0, 1200.0, 8000.0];
    assert!(new.set_crossovers(crossovers));
    let boundary = 12_032;
    let output_fade_start = boundary + 256;
    let mut changed_power = 0.0_f64;
    for (sample, input) in signal(boundary + 1024).into_iter().enumerate() {
        if sample == boundary {
            assert!(changing.set_crossovers(crossovers));
        }
        let old = old.process_frame(input);
        let new = new.process_frame(input);
        let actual = changing.process_frame(input);
        let fade = if sample < output_fade_start {
            0.0
        } else if sample < output_fade_start + 256 {
            (sample - output_fade_start + 1) as f32 / 256.0
        } else {
            1.0
        };
        for ((old, new), actual) in old.into_iter().zip(new).zip(actual) {
            let expected = (new - old).mul_add(fade, old);
            assert!(
                (actual - expected).abs() < 1.0e-6,
                "sample {sample}: {actual} vs {expected}"
            );
            if sample >= output_fade_start {
                changed_power += f64::from(new - old).powi(2);
            }
        }
    }
    assert!(
        changed_power > 0.01,
        "the crossover change must affect the selected band"
    );
}

#[test]
fn independent_source_trims_gate_recovery_and_global_polarity_remains_linear() {
    let mut reference = MultibandImager::new(48_000.0).expect("rate");
    let mut inverted = MultibandImager::new(48_000.0).expect("rate");
    let mut side_off = MultibandImager::new(48_000.0).expect("rate");
    let mut recover_off = MultibandImager::new(48_000.0).expect("rate");
    let controls = [control(0.0, 0.7, 0.25, 0.8, -0.2, false); 4];
    reference.set_controls(controls);
    inverted.set_controls(controls);
    side_off.set_controls([control(0.0, 0.0, 0.25, 1.0, -0.2, false); 4]);
    recover_off.set_controls([control(0.0, 0.7, 0.0, 0.0, -0.2, false); 4]);
    for imager in [&mut reference, &mut inverted, &mut side_off, &mut recover_off] {
        imager.reset();
    }
    let mut recovery_power = 0.0_f64;
    for frame in signal(12_000) {
        let output = reference.process_frame(frame);
        let opposite = inverted.process_frame(frame.map(|value| -value));
        let no_side = side_off.process_frame(frame);
        let no_recovery = recover_off.process_frame(frame);
        for (actual, opposite) in output.into_iter().zip(opposite) {
            assert!((actual + opposite).abs() < 2.0e-6);
            recovery_power += f64::from(actual).powi(2);
        }
        assert!(no_side.into_iter().all(|sample| sample.abs() < f32::EPSILON));
        assert!(no_recovery.into_iter().all(|sample| sample.abs() < f32::EPSILON));
    }
    assert!(recovery_power > 1.0, "Mid Off must leave recovered Side audible");
}

#[test]
fn a_rapid_offline_roundtrip_does_not_lose_the_next_realtime_request() {
    let mut imager = MultibandImager::new(48_000.0).expect("rate");
    assert!(imager.set_crossovers([80.0, 800.0, 8000.0]));
    imager.set_offline(true).expect("offline");
    imager.set_offline(false).expect("realtime");
    let requested = [90.0, 900.0, 9000.0];
    assert!(imager.set_crossovers(requested));
    let deadline = Instant::now() + Duration::from_secs(3);
    while imager.active_crossovers().map(f32::to_bits) != requested.map(f32::to_bits) {
        assert!(Instant::now() < deadline, "request survives mode transition");
        for _ in 0..256 {
            let _ = imager.process_frame([0.0; 2]);
        }
        std::thread::yield_now();
    }
    assert!(!imager.kernel_worker_failed());
}

#[test]
fn realtime_worker_publishes_coherent_requests_without_disturbing_neutral_bits() {
    let mut imager = MultibandImager::new(48_000.0).expect("rate");
    let requested = [200.0, 1000.0, 8000.0];
    assert!(imager.set_crossovers(requested));
    let deadline = Instant::now() + Duration::from_secs(3);
    let frame = [-0.0, 0.123_456_79];
    let mut rendered = 0;
    while imager.active_crossovers().map(f32::to_bits) != requested.map(f32::to_bits) {
        assert!(Instant::now() < deadline, "bounded worker handoff");
        for _ in 0..256 {
            let actual = imager.process_frame(frame);
            if rendered >= imager.latency_samples() {
                assert_eq!(actual.map(f32::to_bits), frame.map(f32::to_bits));
            }
            rendered += 1;
        }
        std::thread::yield_now();
    }
    assert!(!imager.kernel_worker_failed());
}
