//! Independent projection, reference, palette, bounded storage, and weighting contracts for particle construction.

use image_control_dsp::{
    ColorMode, DisplaySettings, FrequencyScale, ParticleBuilder, SpatialAnalyzer, SpectralFrameInfo, SpectralPoint,
    frequency_to_share, share_to_frequency,
};

/// Constructs independent observations without deriving expectations from analyzer output.
fn point(frequency: f32, reassigned: f32, position: f32, power: f32, coherence: f32) -> SpectralPoint {
    SpectralPoint::new(frequency, reassigned, 23.4375, position, power, coherence).expect("valid observation")
}

/// Gives each test an explicit audio clock, never a GUI wall clock.
const fn frame(tier_index: usize, end_seconds: f64, last_at_time: bool) -> SpectralFrameInfo {
    SpectralFrameInfo {
        tier_index,
        frame_seconds: 0.01,
        end_seconds,
        last_at_time,
    }
}

#[test]
fn settings_and_observations_reject_invalid_inputs_before_projection() {
    assert!((DisplaySettings::default().persistence_seconds() - 0.3).abs() < f64::EPSILON);
    assert!((DisplaySettings::default().contrast() - 0.6).abs() < f32::EPSILON);
    for (persistence, contrast) in [
        (f64::NAN, 0.6),
        (0.3, f32::NAN),
        (0.049, 0.6),
        (3.001, 0.6),
        (0.3, 1.01),
    ] {
        assert!(
            DisplaySettings::new(ColorMode::Frequency, FrequencyScale::Logarithmic, persistence, contrast).is_none()
        );
    }
    for fields in [
        [f32::NAN, 1000.0, 10.0, 0.5, 1.0, 1.0],
        [1000.0, f32::INFINITY, 10.0, 0.5, 1.0, 1.0],
        [19.0, 1000.0, 10.0, 0.5, 1.0, 1.0],
        [1000.0, 1000.0, -1.0, 0.5, 1.0, 1.0],
        [1000.0, 1000.0, 10.0, -0.1, 1.0, 1.0],
        [1000.0, 1000.0, 10.0, 0.5, 0.0, 1.0],
        [1000.0, 1000.0, 10.0, 0.5, 1.0, 1.1],
    ] {
        let [frequency, reassigned, spacing, position, power, coherence] = fields;
        assert!(SpectralPoint::new(frequency, reassigned, spacing, position, power, coherence).is_none());
    }
}

#[test]
fn logarithmic_and_htk_mel_projection_match_independent_equations() {
    for scale in [FrequencyScale::Logarithmic, FrequencyScale::Mel] {
        for frequency in [20.0_f64, 80.0, 1000.0, 4000.0, 20_000.0] {
            let expected = match scale {
                FrequencyScale::Logarithmic => (frequency.log10() - 20.0_f64.log10()) / 3.0,
                FrequencyScale::Mel => {
                    let mel = |value: f64| 2595.0 * (1.0 + value / 700.0).log10();
                    (mel(frequency) - mel(20.0)) / (mel(20_000.0) - mel(20.0))
                }
            };
            assert!((frequency_to_share(frequency, scale) - expected).abs() < 1.0e-14);
            assert!((share_to_frequency(expected, scale) - frequency).abs() < 1.0e-9);
            assert_eq!(
                scale.normalized(frequency).to_bits(),
                frequency_to_share(frequency, scale).to_bits()
            );
            assert_eq!(
                scale.frequency(expected).to_bits(),
                share_to_frequency(expected, scale).to_bits()
            );
        }
        let mut builder = ParticleBuilder::new();
        builder.set_settings(DisplaySettings::new(ColorMode::Phase, scale, 0.3, 0.0).expect("settings"));
        builder.add_frame(&[point(1000.0, 1100.0, 0.2, 1.0, 1.0)], frame(0, 0.01, true));
        let particle = builder.particles().first().expect("particle");
        let [x, y] = particle.position();
        assert!((f64::from(x) - frequency_to_share(1100.0, scale)).abs() < 1.0e-7);
        assert!((y - 0.2).abs() < 1.0e-6);
        let expected_spread = (frequency_to_share(23.4375_f64.mul_add(0.5, 1100.0), scale)
            - frequency_to_share(23.4375_f64.mul_add(-0.5, 1100.0), scale))
            * 0.35;
        assert!((f64::from(particle.spread()) - expected_spread).abs() < 1.0e-8);
    }
}

#[test]
fn shared_deadlines_attack_before_coloring_and_release_once_with_retained_tier_peaks() {
    let quiet = point(1000.0, 1000.0, 0.5, 0.01, 1.0);
    let loud = point(1000.0, 1000.0, 0.5, 1.0, 1.0);
    let mut builder = ParticleBuilder::new();
    builder
        .set_settings(DisplaySettings::new(ColorMode::Phase, FrequencyScale::Logarithmic, 0.3, 0.0).expect("settings"));
    builder.add_frame(&[quiet], frame(0, 0.1, false));
    assert!(
        builder.particles().is_empty(),
        "incomplete groups do not expose wrongly scaled particles"
    );
    builder.add_frame(&[loud], frame(5, 0.1, true));
    assert!(builder.reference_level().abs() < 1.0e-6);
    let particles = builder.particles();
    let quiet_particle = particles.first().expect("quiet particle");
    assert!((quiet_particle.emphasis() - 16.0 / 36.0).abs() < 1.0e-6);
    assert!((particles.last().expect("loud particle").emphasis() - 1.0).abs() < 1.0e-6);
    builder.clear();
    builder.add_frame(&[quiet], frame(0, 0.2, true));
    assert!(
        builder.reference_level().abs() < 1.0e-6,
        "slow tier does not erase a retained loud fast tier peak"
    );
    builder.add_frame(&[], frame(5, 0.3, true));
    assert!(
        (builder.reference_level() + 1.2).abs() < 1.0e-6,
        "one audio endpoint releases only once"
    );
    builder.add_frame(&[], frame(0, 0.4, false));
    builder.add_frame(&[], frame(5, 0.4, true));
    assert!((builder.reference_level() + 2.4).abs() < 1.0e-6);
    builder.add_frame(&[], frame(0, 100.0, true));
    assert!((builder.reference_level() + 90.0).abs() < 1.0e-6);
}

#[test]
fn grouped_tier_order_cannot_change_common_reference_or_particle_weight() {
    let mut forward = ParticleBuilder::new();
    let mut reverse = ParticleBuilder::new();
    let quiet = point(1000.0, 1000.0, 0.25, 0.01, 0.5);
    let loud = point(1000.0, 1000.0, 0.75, 1.0, -0.5);
    forward.add_frame(&[quiet], frame(0, 0.02, false));
    forward.add_frame(&[loud], frame(5, 0.02, true));
    reverse.add_frame(&[loud], frame(5, 0.02, false));
    reverse.add_frame(&[quiet], frame(0, 0.02, true));
    let mut reversed = reverse.particles().to_vec();
    reversed.reverse();
    assert_eq!(forward.particles(), reversed);
}

#[test]
fn flash_persistence_phase_colors_and_contrast_match_reference_equations() {
    for (persistence, phase, expected_color) in [
        (0.3, 1.0, [0.1_f32, 0.45, 1.0]),
        (1.2, -1.0, [1.0, 0.07, 0.1]),
        (0.075, 0.0, [0.55, 0.26, 0.55]),
    ] {
        let mut builder = ParticleBuilder::new();
        builder.set_settings(
            DisplaySettings::new(ColorMode::Phase, FrequencyScale::Logarithmic, persistence, 0.6).expect("settings"),
        );
        builder.add_frame(&[point(1000.0, 1000.0, 0.5, 1.0, phase)], frame(0, 0.01, true));
        let particle = builder.particles().first().expect("particle");
        let weight = ((1.0 - (-0.01_f64 / 0.04).exp()) * (0.3 / persistence).sqrt()) as f32;
        for (actual, expected) in particle.color().into_iter().zip(expected_color) {
            assert!(expected.mul_add(-weight, actual).abs() < 1.0e-6);
        }
        assert!((particle.size() - 1.0).abs() < 1.0e-6);
        assert!((particle.emphasis() - 1.0).abs() < 1.0e-6);
    }
    for contrast in [0.0_f32, 0.6, 1.0] {
        for share in [0.2_f32, 0.5, 0.7, 1.0] {
            let mut builder = ParticleBuilder::new();
            builder.set_settings(
                DisplaySettings::new(ColorMode::Phase, FrequencyScale::Logarithmic, 0.3, contrast).expect("settings"),
            );
            let decibels = (share - 1.0) * 36.0;
            builder.add_frame(
                &[
                    point(1000.0, 1000.0, 0.0, 1.0, 1.0),
                    point(1000.0, 1000.0, 1.0, 10.0_f32.powf(decibels / 10.0), 1.0),
                ],
                frame(0, 0.01, true),
            );
            let exponent = 3.0_f32.mul_add(contrast, 1.0);
            let pivot = 0.2_f32.mul_add(contrast, 0.5);
            let odds = (pivot * (1.0 - share) / (1.0 - pivot)).powf(exponent);
            let expected = share.powf(exponent) / (share.powf(exponent) + odds);
            let weight = 1.0 - (-0.25_f32).exp();
            if expected * weight < 1.0e-4 {
                assert_eq!(builder.particles().len(), 1, "sub-threshold particles are culled");
            } else {
                let actual = builder
                    .particles()
                    .last()
                    .expect("visible contrast observation")
                    .emphasis();
                assert!(
                    (actual - expected).abs() < 1.0e-6,
                    "share={share}, contrast={contrast}, actual={actual}, expected={expected}"
                );
            }
        }
    }
}

#[test]
fn frequency_palette_is_linear_light_with_reference_quantization() {
    for frequency in [20.0_f32, 75.0, 1000.0, 8000.0, 20_000.0] {
        let mut builder = ParticleBuilder::new();
        builder.add_frame(&[point(frequency, frequency, 0.5, 1.0, 1.0)], frame(0, 0.01, true));
        let particle = builder.particles().first().expect("particle");
        let step = (f64::from(frequency / 20.0).log(1000.0) * 1023.0) as usize;
        let palette_frequency = 20.0 * 1000.0_f64.powf(step as f64 / 1023.0);
        let stops = [
            (20.0, [1.0, 0.06, 0.04]),
            (50.0, [1.0, 0.1, 0.04]),
            (75.0, [1.0, 0.6, 0.05]),
            (150.0, [1.0, 0.92, 0.1]),
            (400.0, [0.92, 1.0, 0.1]),
            (1000.0, [0.2, 1.0, 0.15]),
            (2500.0, [0.1, 1.0, 0.7]),
            (4000.0, [0.1, 0.9, 1.0]),
            (8000.0, [0.2, 0.45, 1.0]),
            (13_000.0, [0.6, 0.3, 1.0]),
            (20_000.0, [0.95, 0.3, 1.0]),
        ];
        let pair = stops
            .windows(2)
            .find(|pair| {
                pair.last()
                    .is_some_and(|(frequency, _)| *frequency >= palette_frequency)
            })
            .expect("palette interval");
        let &(low_frequency, low) = pair.first().expect("lower");
        let &(high_frequency, high) = pair.last().expect("upper");
        let blend = (palette_frequency / low_frequency).log(high_frequency / low_frequency);
        let linear = |value: f64| {
            if value <= 0.04045 {
                value / 12.92
            } else {
                ((value + 0.055) / 1.055).powf(2.4)
            }
        };
        let weight = 1.0 - (-0.25_f64).exp();
        for ((actual, low), high) in particle.color().into_iter().zip(low).zip(high) {
            let expected = (linear(high) - linear(low)).mul_add(blend, linear(low)) * weight;
            assert!((f64::from(actual) - expected).abs() < 1.0e-6);
        }
    }
}

#[test]
fn overflow_retains_newest_tail_and_drain_reset_reuse_storage() {
    let mut builder = ParticleBuilder::new();
    let observations: Vec<_> = (0..70_000)
        .map(|index| point(1000.0, 1000.0, index as f32 / 70_000.0, 1.0, 1.0))
        .collect();
    builder.add_frame(&observations, frame(0, 0.01, true));
    assert_eq!(builder.particles().len(), 65_536);
    let oldest = builder.particles().first().expect("retained tail");
    assert!((oldest.position().get(1).copied().expect("y") - 4464.0 / 70_000.0).abs() < 1.0e-6);
    builder.add_frame(&[point(1000.0, 1000.0, 1.0, 1.0, 1.0)], frame(0, 0.02, true));
    assert_eq!(builder.particles().len(), 65_536);
    assert!(
        (builder
            .particles()
            .last()
            .expect("newest")
            .position()
            .get(1)
            .copied()
            .expect("y")
            - 1.0)
            .abs()
            < 1.0e-6
    );
    let mut drained = Vec::with_capacity(65_536);
    builder.drain(&mut drained);
    assert_eq!(drained.len(), 65_536);
    assert_eq!(builder.particles(), []);
    builder.reset();
    assert!((builder.reference_level() + 90.0).abs() < 1.0e-6);
    builder.add_frame(&[point(1000.0, 1000.0, 0.0, 1.0, 1.0)], frame(0, 0.01, true));
    builder.drain(&mut drained);
    assert_eq!(drained.len(), 1);
}

#[test]
fn malformed_duplicate_or_backward_metadata_does_not_change_reference_or_particles() {
    let mut builder = ParticleBuilder::new();
    let points = [point(1000.0, 1000.0, 0.5, 1.0, 1.0)];
    for bad in [
        frame(6, 0.1, true),
        frame(0, f64::NAN, true),
        frame(0, -1.0, true),
        SpectralFrameInfo {
            frame_seconds: 0.0,
            ..frame(0, 0.1, true)
        },
    ] {
        builder.add_frame(&points, bad);
    }
    assert_eq!(builder.particles(), []);
    builder.add_frame(&points, frame(0, 0.1, false));
    builder.add_frame(&points, frame(0, 0.1, false));
    builder.add_frame(&points, frame(5, 0.1, true));
    assert_eq!(builder.particles().len(), 2);
    builder.add_frame(&points, frame(0, 0.05, true));
    builder.add_frame(&points, frame(5, 0.1, true));
    assert_eq!(builder.particles().len(), 2);
}

#[test]
fn octave_tilt_and_high_frequency_contrast_match_independent_display_equations() {
    let mut builder = ParticleBuilder::new();
    let frequencies = [125.0_f32, 1000.0, 4000.0, 16_000.0];
    let points: Vec<_> = frequencies
        .into_iter()
        .map(|frequency| {
            // Each observation has a tilted level of -18 dB, independent of source frequency.
            let tilt = 3.0 * (frequency / 1000.0).log2();
            point(frequency, frequency, 0.5, 10.0_f32.powf((-18.0 - tilt) / 10.0), 1.0)
        })
        .collect();
    builder.add_frame(&[point(1000.0, 1000.0, 0.5, 1.0, 1.0)], frame(0, 0.01, true));
    builder.clear();
    builder.add_frame(&points, frame(1, 0.02, true));
    assert!(
        builder.reference_level().abs() < 1.0e-6,
        "retained reference anchors the display range"
    );
    assert_eq!(builder.particles().len(), 4);
    for (frequency, particle) in frequencies.into_iter().zip(builder.particles()) {
        let ramp = ((frequency / 1000.0).log2() / 4.0).clamp(0.0, 1.0);
        let contrast = 0.25_f32.mul_add(ramp, 0.6);
        let exponent = 3.0_f32.mul_add(contrast, 1.0);
        let pivot = 0.2_f32.mul_add(contrast, 0.5);
        let rising = 0.5_f32.powf(exponent);
        let falling = (pivot * 0.5 / (1.0 - pivot)).powf(exponent);
        assert!((particle.emphasis() - rising / (rising + falling)).abs() < 1.0e-6);
    }
}

#[test]
fn production_analyzer_to_particles_is_chunk_independent() {
    let input: Vec<_> = (0..8192)
        .map(|sample| {
            let phase = std::f64::consts::TAU * 1111.0 * f64::from(sample) / 48_000.0;
            [phase.sin() as f32, (phase + 0.8).sin() as f32 * 0.3]
        })
        .collect();
    let render = |chunk_size| {
        let mut analyzer = SpatialAnalyzer::new(48_000.0).expect("rate");
        let mut builder = ParticleBuilder::new();
        for chunk in input.chunks(chunk_size) {
            analyzer
                .push_frames(chunk, |points, frame| builder.add_frame(points, frame))
                .expect("analysis");
        }
        (builder.reference_level(), builder.particles().to_vec())
    };
    assert_eq!(render(input.len()), render(17));
}
