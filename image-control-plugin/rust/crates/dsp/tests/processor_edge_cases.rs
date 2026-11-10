//! Regression contracts for Solo-aware Delta, exact Mono endpoints, and truthful preparation rates.

use image_control_dsp::{Imager, Parameters};

/// Constructs a prepared offline processor with deterministic kernel replacement.
fn prepared() -> Imager {
    let mut imager = Imager::new();
    imager.set_offline(true);
    assert!(imager.prepare(48_000.0));
    imager
}

#[test]
fn settled_mono_assigns_identical_midpoint_bits_for_cancellation_sensitive_input() {
    let mut imager = prepared();
    imager.shared().set_mono(true);
    imager.reset();
    let frame = [1.0, -0.999_999_94];
    let expected = f32::midpoint(frame[0], frame[1]);
    let mut output = [0.0; 2];
    for _ in 0..=imager.latency_samples() {
        output = imager.process_frame(frame);
    }
    assert_eq!(output.map(f32::to_bits), [expected.to_bits(); 2]);
}

#[test]
fn automated_mono_lands_on_identical_channels_at_the_endpoint() {
    let mut imager = prepared();
    let input = [1.0, -0.999_999_94];
    for _ in 0..=imager.latency_samples() {
        let _ = imager.process_frame(input);
    }
    imager.shared().set_mono(true);
    imager.begin_block();
    for _ in 0..960 {
        let _ = imager.process_frame(input);
    }
    let output = imager.process_frame(input);
    assert_eq!(output[0].to_bits(), output[1].to_bits());
    assert_eq!(output[0].to_bits(), f32::midpoint(input[0], input[1]).to_bits());
}

#[test]
fn unsupported_preparation_keeps_the_previous_rate_and_audio_history() {
    let mut candidate = prepared();
    let mut reference = prepared();
    for _ in 0..257 {
        assert_eq!(
            candidate.process_frame([0.75, -0.25]),
            reference.process_frame([0.75, -0.25])
        );
    }
    for rate in [0.0, 0.5, -1.0, 2_822_401.0, f32::NAN, f32::INFINITY] {
        assert!(!candidate.prepare(rate), "rate {rate} must not be silently clamped");
        assert_eq!(candidate.shared().sample_rate().to_bits(), 48_000.0_f32.to_bits());
        assert_eq!(candidate.latency_samples(), reference.latency_samples());
    }
    for _ in 0..5_000 {
        assert_eq!(
            candidate.process_frame([0.125, -0.5]),
            reference.process_frame([0.125, -0.5])
        );
    }
}

#[test]
fn preparation_uses_actual_low_and_ultrahigh_rates() {
    for (rate, expected_latency, expected_tail) in [
        (1.0, 4_352, 4_096),
        (8_000.0, 4_352, 4_096),
        (768_000.0, 65_792, 65_536),
        (1_536_000.0, 131_328, 131_072),
    ] {
        let mut imager = Imager::new();
        assert!(imager.prepare(rate));
        assert_eq!(imager.shared().sample_rate().to_bits(), rate.to_bits());
        assert_eq!(imager.latency_samples(), expected_latency);
        assert_eq!(imager.tail_samples(), expected_tail);
    }
}

#[test]
fn solo_delta_is_silent_for_unchanged_selected_bands() {
    for solo in 0..4 {
        let mut imager = prepared();
        imager.shared().set_band_parameter(solo, 5, 1.0);
        imager.shared().set_delta(true);
        imager.reset();
        for index in 0..12_288 {
            let frame = [(index as f32 * 0.07).sin(), (index as f32 * 0.13).cos()];
            let output = imager.process_frame(frame);
            assert!(
                output.iter().all(|sample| sample.abs() < 1.0e-6),
                "solo {solo}, frame {index}: {output:?}"
            );
        }
    }
}

#[test]
fn selected_band_delta_retains_a_real_output_gain_difference() {
    let mut dry = prepared();
    let mut delta = prepared();
    for imager in [&mut dry, &mut delta] {
        imager.shared().set_band_parameter(2, 5, 1.0);
        imager.reset();
    }
    delta.shared().set_parameters(Parameters::new(0.5, 1.0).expect("gain"));
    delta.shared().set_delta(true);
    delta.reset();
    let mut peak = 0.0_f32;
    for index in 0..12_288 {
        let frame = [(index as f32 * 0.17).sin(), (index as f32 * 0.23).cos()];
        let selected = dry.process_frame(frame);
        let difference = delta.process_frame(frame);
        for (actual, reference) in difference.into_iter().zip(selected) {
            assert!(reference.mul_add(0.5, actual).abs() < 1.0e-6);
            peak = peak.max(actual.abs());
        }
    }
    assert!(peak > 0.1, "Delta must not suppress a real processing difference");
}

#[test]
fn solo_delta_reference_follows_audible_ramps_and_crossover_handoffs() {
    let mut imager = prepared();
    imager.shared().set_delta(true);
    imager.reset();
    for index in 0..20_480 {
        if index == 6_000 {
            imager.shared().set_band_parameter(1, 5, 1.0);
        }
        if index == 9_000 {
            imager.shared().set_crossover(1, 1_200.0);
        }
        if index == 12_000 {
            imager.shared().set_band_parameter(2, 5, 1.0);
        }
        if index == 15_000 {
            imager.shared().set_band_parameter(1, 5, 0.0);
        }
        if index % 73 == 0 {
            imager.begin_block();
        }
        let frame = [(index as f32 * 0.09).sin(), (index as f32 * 0.017).cos()];
        let output = imager.process_frame(frame);
        assert!(
            output.iter().all(|sample| sample.abs() < 1.0e-6),
            "frame {index}: {output:?}"
        );
    }
}
