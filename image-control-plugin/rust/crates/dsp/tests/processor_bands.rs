//! End-to-end contracts for prepared multiband transport, exact neutrality, and independent band controls.

use image_control_dsp::{Imager, Parameters};

/// Constructs a prepared deterministic offline processor with current targets settled.
fn prepared() -> Imager {
    let mut imager = Imager::new();
    imager.set_offline(true);
    assert!(imager.prepare(48_000.0));
    imager
}

#[test]
fn prepared_neutral_copies_delayed_channel_bits_exactly() {
    let mut imager = prepared();
    let latency = imager.latency_samples() as usize;
    assert_eq!(latency, 4_352);
    assert_eq!(imager.tail_samples(), 4_096);
    let input: Vec<_> = (0..8_192)
        .map(|index| {
            if index == 0 {
                [-0.0, 0.0]
            } else {
                [index as f32 * 0.00001, -(index as f32) * 0.00003]
            }
        })
        .collect();
    imager.begin_block();
    for index in 0..input.len() + latency {
        let frame = input.get(index).copied().unwrap_or_default();
        let output = imager.process_frame(frame);
        if let Some(expected) = index.checked_sub(latency).and_then(|index| input.get(index)) {
            assert_eq!(output.map(f32::to_bits), expected.map(f32::to_bits));
        } else {
            assert!(output.iter().all(|sample| sample.abs() < f32::EPSILON));
        }
    }
}

#[test]
fn saved_crossovers_apply_to_the_first_prering_not_a_late_worker_result() {
    let make = |frequency: f32| {
        let mut imager = Imager::new();
        imager.shared().set_crossover(0, frequency);
        imager.shared().set_band_parameter(0, 5, 1.0);
        assert!(imager.prepare(48_000.0));
        (0..9_000)
            .map(|index| imager.process_frame(if index == 0 { [1.0; 2] } else { [0.0; 2] })[0])
            .collect::<Vec<_>>()
    };
    let low = make(50.0);
    let higher = make(300.0);
    assert!(
        low.iter()
            .zip(higher)
            .any(|(left, right)| (*left - right).abs() > 0.001)
    );
}

#[test]
fn saved_band_off_values_mute_without_changing_latency() {
    let mut imager = prepared();
    for band in 0..4 {
        imager.shared().set_band_parameter(band, 0, 0.0);
        imager.shared().set_band_parameter(band, 1, 0.0);
    }
    imager.reset();
    imager.begin_block();
    for index in 0..16_384 {
        let frame = [(index as f32 * 0.07).sin(), (index as f32 * 0.1).cos()];
        assert!(imager.process_frame(frame).iter().all(|sample| sample.abs() < 1e-6));
    }
    assert_eq!(imager.latency_samples(), 4_352);
}

#[test]
fn bypass_restores_delayed_input_and_exact_neutral_delta_is_zero() {
    let mut imager = prepared();
    imager
        .shared()
        .set_parameters(Parameters::new(0.25, 0.0).expect("finite"));
    imager.shared().set_bypass(true);
    imager.reset();
    imager.begin_block();
    let latency = imager.latency_samples() as usize;
    for index in 0..latency + 2_048 {
        let frame = if index == 0 { [0.75, -0.25] } else { [0.0; 2] };
        let output = imager.process_frame(frame);
        if index == latency {
            assert_eq!(output, [0.75, -0.25]);
        }
    }
    imager.shared().set_parameters(Parameters::default());
    imager.shared().set_bypass(false);
    imager.shared().set_delta(true);
    imager.reset();
    imager.begin_block();
    for index in 0..latency + 4_096 {
        assert_eq!(imager.process_frame([(index as f32 * 0.1).sin(), 0.25]), [0.0; 2]);
    }
}
