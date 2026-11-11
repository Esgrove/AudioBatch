//! Analytical, cross-rate sine calibration and configurable spatial-curve contracts for every analysis tier.

use image_control_dsp::{SpatialAnalyzer, StereoPositionCurve};

#[test]
fn native_bin_sines_calibrate_to_one_in_every_tier_at_44k_and_96k() {
    for (rate, scale) in [(44_100.0_f32, 1), (96_000.0, 2)] {
        for (tier, (window, native_bin)) in [
            (32_768, 32),
            (16_384, 40),
            (8192, 40),
            (4096, 40),
            (2048, 64),
            (1024, 192),
        ]
        .into_iter()
        .enumerate()
        {
            let window = window * scale;
            let frequency = f64::from(rate) * f64::from(native_bin) / f64::from(window);
            let input: Vec<_> = (0..window)
                .map(|index| {
                    let phase = std::f64::consts::TAU * frequency * f64::from(index) / f64::from(rate);
                    [phase.sin() as f32, 0.0]
                })
                .collect();
            let mut analyzer = SpatialAnalyzer::new(rate).expect("supported rate");
            let mut observed = None;
            analyzer
                .push_frames(&input, |points, frame| {
                    if frame.tier_index == tier {
                        observed = points
                            .iter()
                            .max_by(|first, second| first.power().total_cmp(&second.power()))
                            .copied();
                    }
                })
                .expect("stream");
            let peak = observed.expect("tier sine peak");
            assert!(
                (peak.power() - 1.0).abs() < 2.0e-6,
                "rate={rate}, tier={tier}: {}",
                peak.power()
            );
            assert!((f64::from(peak.frequency()) - frequency).abs() < 0.001);
            assert!(peak.position() < 1.0e-6);
        }
    }
}

#[test]
fn bass_tier_resolves_40_and_45_hertz_at_independent_stereo_positions() {
    let input: Vec<_> = (0..32_768)
        .map(|index| {
            let time = f64::from(index) / 48_000.0;
            [
                (std::f64::consts::TAU * 40.0 * time).sin() as f32,
                (std::f64::consts::TAU * 45.0 * time).sin() as f32,
            ]
        })
        .collect();
    let mut analyzer = SpatialAnalyzer::new(48_000.0).expect("rate");
    let mut observed = Vec::new();
    analyzer
        .push_frames(&input, |points, frame| {
            if frame.tier_index == 0 {
                observed.clear();
                observed.extend_from_slice(points);
            }
        })
        .expect("stream");
    for (frequency, left) in [(40.0_f32, true), (45.0, false)] {
        let peak = observed
            .iter()
            .filter(|point| (point.frequency() - frequency).abs() < 1.0)
            .max_by(|first, second| first.power().total_cmp(&second.power()))
            .expect("resolved bass peak");
        assert!((peak.reassigned_frequency() - frequency).abs() < 0.1);
        assert!(if left {
            peak.position() < 0.01
        } else {
            peak.position() > 0.99
        });
        assert!(peak.power() > 0.8);
    }
}

#[test]
fn default_and_configured_midpoint_and_edge_settings_have_analytical_positions() {
    for (midpoint, edge) in [(12.0, 32.0), (6.0, 18.0), (24.0, 60.0)] {
        for (difference, share) in [(midpoint, 0.5_f32), (edge, 0.99)] {
            for left_louder in [true, false] {
                let quiet = 10.0_f64.powf(-difference / 20.0);
                let input: Vec<_> = (0..4096)
                    .map(|sample| {
                        let value = (std::f64::consts::TAU * 750.0 * f64::from(sample) / 48_000.0).sin();
                        if left_louder {
                            [value as f32, (value * quiet) as f32]
                        } else {
                            [(value * quiet) as f32, value as f32]
                        }
                    })
                    .collect();
                let mut analyzer = SpatialAnalyzer::new(48_000.0).expect("rate");
                analyzer.set_position_curve(StereoPositionCurve::new(midpoint, edge).expect("valid curve"));
                let mut observed = None;
                analyzer
                    .push_frames(&input, |points, frame| {
                        if frame.tier_index == 4 {
                            observed = points
                                .iter()
                                .max_by(|first, second| first.power().total_cmp(&second.power()))
                                .copied();
                        }
                    })
                    .expect("stream");
                let expected = if left_louder {
                    (1.0 - share) * 0.5
                } else {
                    1.0_f32.midpoint(share)
                };
                assert!((observed.expect("peak").position() - expected).abs() < 2.0e-6);
            }
        }
    }
}
