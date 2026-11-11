//! Analytical estimator contracts and production band-observation transparency, controls, and lifecycle tests.

#[path = "../src/correlation.rs"]
mod correlation;

use correlation::BandCorrelation;
use image_control_dsp::{BandControls, MultibandImager};

/// Builds validated plain-unit band controls without bypassing the production parameter ranges.
fn control(mid: f32, side: f32, width: f32, recover: f32, pan: f32, solo: bool) -> BandControls {
    BandControls::new(mid, side, width, recover, pan, solo).expect("finite controls")
}

/// Checks a finite correlation against an independent analytical expectation.
fn close(actual: f32, expected: f64, tolerance: f64) {
    assert!(actual.is_finite(), "expected {expected}, got unavailable");
    assert!(
        (f64::from(actual) - expected).abs() < tolerance,
        "correlation {actual}, expected {expected} within {tolerance}"
    );
}

/// Excites all four crossover regions with independent cosine Mid and sine Side components.
fn band_signal(sample: usize) -> [f32; 2] {
    let (mid, side) = [40.0, 250.0, 2000.0, 12_000.0]
        .into_iter()
        .fold((0.0, 0.0), |(mid, side), frequency| {
            let phase = std::f64::consts::TAU * frequency * sample as f64 / 48_000.0;
            (phase.cos().mul_add(0.1, mid), phase.sin().mul_add(0.1, side))
        });
    [(mid + side) as f32, (mid - side) as f32]
}

/// Feeds one second of analytical input into the production convolution and correlation path.
fn feed(imager: &mut MultibandImager, input: impl Fn(usize) -> [f32; 2]) {
    for sample in 0..48_128 {
        let _ = imager.process_frame(input(sample));
    }
}

#[test]
fn centered_estimator_distinguishes_polarity_orthogonality_and_dc_offsets() {
    let mut correlation = BandCorrelation::new(8000.0);
    for sample in 0..48_000 {
        let phase = std::f64::consts::TAU * 200.0 * f64::from(sample) / 8000.0;
        let sine = phase.sin() as f32;
        let cosine = phase.cos() as f32;
        correlation.push(0, sine, sine * 0.3);
        correlation.push(1, sine, sine * -0.7);
        correlation.push(2, sine, cosine);
        correlation.push(3, sine + 2.0, cosine - 3.0);
    }
    correlation.publish();
    let [positive, negative, orthogonal, offset] = correlation.readings();
    close(positive, 1.0, 1.0e-6);
    close(negative, -1.0, 1.0e-6);
    close(orthogonal, 0.0, 0.004);
    close(offset, f64::from(orthogonal), 1.0e-6);

    correlation.reset();
    for _ in 0..48_000 {
        correlation.push(0, 1.0, -2.0);
    }
    correlation.publish();
    assert!(
        correlation.readings().into_iter().all(f32::is_nan),
        "DC is not channel variance"
    );
}

#[test]
fn silence_one_sided_low_power_and_non_finite_samples_are_unavailable() {
    let mut correlation = BandCorrelation::new(8000.0);
    assert!(correlation.readings().into_iter().all(f32::is_nan));
    let initial_revision = correlation.revision();
    for sample in 0..8000 {
        let value = if sample % 2 == 0 { 1.0 } else { -1.0 };
        correlation.push(0, 0.0, 0.0);
        correlation.push(1, value, 0.0);
        correlation.push(2, value * 1.0e-6, value * 1.0e-6);
        correlation.push(3, value * 2.0e-5, value * 2.0e-5);
    }
    correlation.publish();
    assert_eq!(correlation.revision(), initial_revision + 1);
    let [silent, one_sided, low, sufficient] = correlation.readings();
    assert!([silent, one_sided, low].into_iter().all(f32::is_nan));
    close(sufficient, 1.0, 1.0e-6);
    for bad in [f32::NAN, f32::INFINITY, f32::NEG_INFINITY] {
        correlation.push(3, bad, 1.0);
        correlation.publish();
        assert!(correlation.readings().into_iter().all(f32::is_nan));
        for value in [1.0, -1.0] {
            correlation.push(3, value, value);
        }
        correlation.publish();
        close(correlation.readings().last().copied().expect("four bands"), 1.0, 1.0e-6);
        correlation.push(3, 1.0, bad);
        correlation.publish();
        assert!(correlation.readings().into_iter().all(f32::is_nan));
    }
    correlation.reset();
    assert!(correlation.readings().into_iter().all(f32::is_nan));
}

#[test]
fn integration_time_is_150_milliseconds_at_every_prepared_rate() {
    for rate in [8000_u32, 44_100, 48_000, 96_000, 192_000] {
        let mut correlation = BandCorrelation::new(rate as f32);
        // Alternating signs give known unit second moments and negligible bounded exponentially weighted means.
        for sample in 0..rate * 4 {
            let value = if sample % 2 == 0 { 1.0 } else { -1.0 };
            correlation.push(0, value, value);
        }
        for step in 1..=3 {
            let count = rate * 15 / 100;
            for sample in 0..count {
                let value = if sample % 2 == 0 { 1.0 } else { -1.0 };
                correlation.push(0, value, -value);
            }
            correlation.publish();
            let elapsed = f64::from(count * step) / f64::from(rate);
            let expected = 2.0_f64.mul_add((-elapsed / 0.15).exp(), -1.0);
            close(
                correlation.readings().first().copied().expect("first band"),
                expected,
                2.0e-6,
            );
        }
    }
}

#[test]
fn neutral_bands_measure_positive_negative_and_near_zero_correlation() {
    let mut imager = MultibandImager::new(48_000.0).expect("rate");
    imager.set_correlation_enabled(true);
    for polarity in [1.0, -1.0] {
        imager.reset();
        feed(&mut imager, |sample| {
            let [left, right] = band_signal(sample);
            let mid = f32::midpoint(left, right);
            [mid, mid * polarity]
        });
        for reading in imager.correlation_readings() {
            close(reading, f64::from(polarity), 1.0e-6);
        }
    }
    imager.reset();
    feed(&mut imager, |sample| {
        let [left, right] = band_signal(sample);
        [f32::midpoint(left, right), (left - right) * 0.5]
    });
    for reading in imager.correlation_readings() {
        close(reading, 0.0, 0.025);
    }
}

/// Evaluates the analytical covariance of transformed cosine and sine coefficients, not DSP output samples.
fn analytical_correlation(mid: f64, side: f64, width: f64, recover: f64, pan: f64) -> f64 {
    // Q(sin) = -cos at the common FIR delay, so recovery subtracts from this source's cosine Mid.
    let mid = (side * recover).mul_add(-(1.0 - width).max(0.0), mid);
    let side = side * width;
    let left = [mid, side];
    let right = [mid, -side];
    let to_right = pan.max(0.0);
    let to_left = (-pan).max(0.0);
    let left = std::array::from_fn::<_, 2, _>(|index| {
        right.get(index).copied().expect("coefficient").mul_add(
            to_left,
            left.get(index).copied().expect("coefficient") * (1.0 - to_right),
        )
    });
    let right = [
        mid.mul_add(to_right, mid * (1.0 - to_left)),
        side.mul_add(to_right, -side * (1.0 - to_left)),
    ];
    let dot = |first: [f64; 2], second: [f64; 2]| first.into_iter().zip(second).map(|(a, b)| a * b).sum::<f64>();
    dot(left, right) / (dot(left, left) * dot(right, right)).sqrt()
}

#[test]
fn each_band_observes_source_trims_width_recovery_and_signal_transfer_pan() {
    let settings = [
        [0.8, 0.6, 0.7, 0.0, 0.3],
        [0.0, 0.9, 0.25, 0.8, -0.4],
        [1.1, 0.4, 1.5, 1.0, 0.1],
        [0.6, 0.8, 0.0, 1.0, -0.2],
    ];
    let mut imager = MultibandImager::new(48_000.0).expect("rate");
    imager
        .set_controls(settings.map(|[mid, side, width, recover, pan]| control(mid, side, width, recover, pan, false)));
    imager.set_correlation_enabled(true);
    imager.reset();
    feed(&mut imager, band_signal);
    for (reading, [mid, side, width, recover, pan]) in imager.correlation_readings().into_iter().zip(settings) {
        let expected = analytical_correlation(
            f64::from(mid),
            f64::from(side),
            f64::from(width),
            f64::from(recover),
            f64::from(pan),
        );
        close(reading, expected, 0.025);
    }
}

#[test]
fn solo_silence_and_fully_panned_bands_are_unavailable() {
    let mut imager = MultibandImager::new(48_000.0).expect("rate");
    imager.set_correlation_enabled(true);
    for settings in [
        std::array::from_fn(|band| control(1.0, 1.0, 0.0, 0.0, 0.0, band == 1)),
        [control(0.0, 0.0, 1.0, 1.0, 0.0, false); 4],
        [control(1.0, 1.0, 1.0, 0.0, 1.0, false); 4],
        [control(1.0, 1.0, 1.0, 0.0, -1.0, false); 4],
    ] {
        imager.set_controls(settings);
        imager.reset();
        feed(&mut imager, band_signal);
        for (band, reading) in imager.correlation_readings().into_iter().enumerate() {
            if settings.get(band).is_some_and(|control| control.solo()) {
                close(reading, 1.0, 1.0e-6);
            } else {
                assert!(reading.is_nan(), "band {band}: {reading}");
            }
        }
    }
    imager.set_controls([control(1.0, 1.0, 0.0, 0.0, 0.0, true); 4]);
    imager.reset();
    feed(&mut imager, band_signal);
    for reading in imager.correlation_readings() {
        close(reading, 1.0, 1.0e-6);
    }
}

#[test]
fn mono_bands_ignore_stereo_only_controls_and_lifecycle_changes_invalidate_readings() {
    let mut imager = MultibandImager::new(48_000.0).expect("rate");
    assert!(imager.set_channels(1));
    imager.set_controls([control(0.7, 0.0, 0.0, 1.0, 1.0, false); 4]);
    imager.set_correlation_enabled(true);
    imager.reset();
    feed(&mut imager, |sample| {
        let [left, _] = band_signal(sample);
        [left, f32::NAN]
    });
    for reading in imager.correlation_readings() {
        close(reading, 1.0, 1.0e-6);
    }
    let revision = imager.correlation_revision();
    imager.set_correlation_enabled(true);
    assert_eq!(
        revision,
        imager.correlation_revision(),
        "unchanged enablement preserves moments"
    );
    assert!(imager.set_channels(2));
    assert!(imager.correlation_readings().into_iter().all(f32::is_nan));
    assert_eq!(imager.correlation_revision(), revision + 1);
    imager.set_correlation_enabled(false);
    assert!(imager.correlation_readings().into_iter().all(f32::is_nan));
    let revision = imager.correlation_revision();
    feed(&mut imager, band_signal);
    assert_eq!(
        imager.correlation_revision(),
        revision,
        "disabled processing does not publish"
    );
    imager.set_correlation_enabled(true);
    assert_eq!(imager.correlation_revision(), revision + 1);
    assert!(imager.correlation_readings().into_iter().all(f32::is_nan));
    for _ in 0..255 {
        let _ = imager.process_frame([0.0; 2]);
    }
    assert_eq!(imager.correlation_revision(), revision + 1);
    let _ = imager.process_frame([0.0; 2]);
    assert_eq!(
        imager.correlation_revision(),
        revision + 2,
        "one publication per partition"
    );
    imager.reset();
    assert!(imager.correlation_readings().into_iter().all(f32::is_nan));
}

#[test]
fn observed_ramps_match_the_selected_audible_band_without_advancing_controls_twice() {
    let mut imager = MultibandImager::new(48_000.0).expect("rate");
    imager.set_controls(std::array::from_fn(|band| control(0.8, 0.6, 0.3, 0.7, 0.2, band == 2)));
    imager.reset();
    imager.set_correlation_enabled(true);
    let mut observed_output = BandCorrelation::new(48_000.0);
    let mut previous_reading = f32::NAN;
    for sample in 0..20_480 {
        if sample == 9011 || sample == 12_199 {
            let target = if sample == 9011 { 0.9 } else { 0.2 };
            imager.set_controls(std::array::from_fn(|band| {
                control(0.4, 1.1, target, 0.9, -0.4, band == 2)
            }));
        }
        let [left, right] = imager.process_frame(band_signal(sample));
        observed_output.push(0, left, right);
        if (sample + 1) % 256 == 0 {
            observed_output.publish();
            let measured_output = observed_output.readings().first().copied().expect("first band");
            if previous_reading.is_nan() {
                assert!(measured_output.is_nan());
            } else {
                close(measured_output, f64::from(previous_reading), 1.0e-6);
            }
            previous_reading = imager.correlation_readings().get(2).copied().expect("selected band");
        }
    }
}

#[test]
fn enabled_disabled_and_toggled_meters_preserve_every_audio_and_reference_bit() {
    for channels in [1, 2] {
        let mut reference = MultibandImager::new(48_000.0).expect("rate");
        let mut monitored = MultibandImager::new(48_000.0).expect("rate");
        for imager in [&mut reference, &mut monitored] {
            assert!(imager.set_channels(channels));
            imager.set_offline(true).expect("offline");
        }
        monitored.set_correlation_enabled(true);
        for sample in 0_usize..26_624 {
            if [2017, 7033, 13_071, 18_011].contains(&sample) {
                let settings = match sample {
                    // Recover ramps remain inaudible at unity Width, including on the neutral fast path.
                    2017 => [control(1.0, 1.0, 1.0, 1.0, 0.0, false); 4],
                    7033 => std::array::from_fn(|band| {
                        control((band as f32).mul_add(0.2, 0.3), 0.7, 0.4, 0.8, -0.3, band % 2 == 0)
                    }),
                    13_071 => [control(0.0, 1.1, 0.0, 1.0, 0.5, false); 4],
                    _ => [BandControls::default(); 4],
                };
                reference.set_controls(settings);
                monitored.set_controls(settings);
            }
            if [1111, 9099, 21_009].contains(&sample) {
                let crossovers = if sample == 9099 {
                    [220.0, 1200.0, 8000.0]
                } else {
                    [80.0, 700.0, 6000.0]
                };
                assert!(reference.set_crossovers(crossovers));
                assert!(monitored.set_crossovers(crossovers));
            }
            if sample == 10_003 {
                monitored.set_correlation_enabled(false);
            } else if sample == 11_019 {
                monitored.set_correlation_enabled(true);
            }
            let input = if sample % 97 == 0 {
                [-0.0, 0.0]
            } else {
                band_signal(sample)
            };
            let expected = reference.process_frame(input);
            let actual = monitored.process_frame(input);
            assert_eq!(actual.map(f32::to_bits), expected.map(f32::to_bits), "frame {sample}");
            assert_eq!(
                monitored.dry_reference().map(f32::to_bits),
                reference.dry_reference().map(f32::to_bits),
                "reference frame {sample}"
            );
            if sample >= 19_500 {
                let source = sample - monitored.latency_samples();
                let [left, right] = if source.is_multiple_of(97) {
                    [-0.0, 0.0]
                } else {
                    band_signal(source)
                };
                assert_eq!(
                    actual.map(f32::to_bits),
                    [left, if channels == 2 { right } else { left }].map(f32::to_bits)
                );
            }
        }
    }
}
