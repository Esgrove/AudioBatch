//! Public parameter contracts independent of the native editor and host framework.

use image_control_dsp::Imager;
use image_control_plugin_rs::params::{
    BAND_CONTROL_COUNT, BAND_PARAMETER_START, BYPASS_PARAMETER, CROSSOVER_PARAMETER_START, DELTA_PARAMETER,
    EditorBinding, GAIN_PARAMETER, MONO_PARAMETER, NEUTRAL_GAIN, PARAMETER_COUNT, Parameters, Snapshot,
    band_parameter_id, parameter_spec,
};

#[test]
fn registry_is_exactly_31_stable_parameters_with_no_global_width() {
    assert_eq!(PARAMETER_COUNT, 31);
    assert_eq!(GAIN_PARAMETER, 0);
    assert_eq!(CROSSOVER_PARAMETER_START, 1);
    assert_eq!(BAND_PARAMETER_START, 4);
    assert_eq!([BYPASS_PARAMETER, DELTA_PARAMETER, MONO_PARAMETER], [28, 29, 30]);
    let parameters = Parameters::new();
    for id in 0..PARAMETER_COUNT as u32 {
        let spec = parameter_spec(id).expect("declared parameter");
        assert_eq!(spec.id, id);
        assert_eq!(
            parameters.get(id).expect("known id").to_bits(),
            spec.default_normalized.to_bits()
        );
        assert!(spec.title != "Width", "there is no global width");
    }
    assert!(parameter_spec(31).is_none());
    assert!(parameters.get(31).is_none());
    for band in 0..4 {
        for field in 0..BAND_CONTROL_COUNT {
            assert_eq!(band_parameter_id(band, field), Some(4 + (band * 6 + field) as u32));
        }
    }
    assert!(band_parameter_id(4, 0).is_none());
    assert!(band_parameter_id(0, 6).is_none());
}

#[test]
fn plain_ranges_defaults_and_text_are_processing_units() {
    for (id, minimum, default, maximum) in [
        (GAIN_PARAMETER, -24.0, 0.0, 12.0),
        (4, -60.0, 0.0, 12.0),
        (5, -60.0, 0.0, 12.0),
        (6, 0.0, 1.0, 2.0),
        (7, 0.0, 0.0, 200.0),
        (8, -100.0, 0.0, 100.0),
        (9, 0.0, 0.0, 1.0),
    ] {
        let spec = parameter_spec(id).expect("spec");
        assert_eq!(spec.normalized_to_plain(0.0), minimum);
        assert_eq!(spec.normalized_to_plain(spec.default_normalized), default);
        assert_eq!(spec.normalized_to_plain(1.0), maximum);
        assert_eq!(spec.plain_to_normalized(minimum), 0.0);
        assert_eq!(spec.plain_to_normalized(maximum), 1.0);
    }
    for (index, default) in [100.0, 500.0, 5000.0].into_iter().enumerate() {
        let spec = parameter_spec(CROSSOVER_PARAMETER_START + index as u32).expect("crossover");
        assert!((spec.normalized_to_plain(spec.default_normalized) - default).abs() < default * 2e-14);
    }
    for (id, text, plain) in [
        (0, "-12 dB", -12.0),
        (1, "1.5 kHz", 1500.0),
        (7, "25%", 25.0),
        (7, "100%", 100.0),
        (7, "150%", 150.0),
        (7, "200%", 200.0),
        (8, "40L", -40.0),
        (8, "C", 0.0),
    ] {
        let spec = parameter_spec(id).expect("spec");
        assert!((spec.normalized_to_plain(spec.parse(text).expect("valid text")) - plain).abs() < 1e-10);
    }
    for id in 0..PARAMETER_COUNT as u32 {
        let spec = parameter_spec(id).expect("spec");
        for text in ["NaN", "inf", "not a number"] {
            assert!(spec.parse(text).is_none(), "{id}: {text}");
        }
        for normalized in [0.0, spec.default_normalized, 1.0] {
            let text = spec.format(normalized);
            let parsed = spec.parse(&text).expect("formatted text parses");
            assert!((parsed - normalized).abs() < 1e-5, "{id}: {text}");
        }
    }
}

#[test]
fn recover_uses_the_full_200_percent_host_and_numeric_input_domain_for_every_band() {
    for band in 0..4 {
        let id = band_parameter_id(band, 3).expect("Recover id");
        let spec = parameter_spec(id).expect("Recover spec");
        assert_eq!(spec.units, "%");
        assert_eq!(spec.default_normalized, 0.0);
        for (normalized, percent) in [(0.0, 0.0), (0.25, 50.0), (0.5, 100.0), (0.75, 150.0), (1.0, 200.0)] {
            assert_eq!(spec.normalized_to_plain(normalized), percent);
            assert_eq!(spec.plain_to_normalized(percent), normalized);
            assert_eq!(spec.parse(&format!("{percent}%")), Some(normalized));
            assert_eq!(spec.parse(&spec.format(normalized)), Some(normalized));
        }
        for text in ["NaN%", "inf%", "10%%"] {
            assert!(spec.parse(text).is_none(), "{text}");
        }
        assert_eq!(spec.parse("-1%"), Some(0.0));
        assert_eq!(spec.parse("200.1%"), Some(1.0));
        assert_eq!(spec.parse("201%"), Some(1.0));
        assert_eq!(spec.plain_to_normalized(201.0), 1.0);
    }
}

/// Decimal targets specify editing precision independently of the implementation's arithmetic.
#[test]
fn plain_unit_steps_apply_to_host_edits_and_text_with_upper_midpoint_ties() {
    let parameters = Parameters::new();
    for (id, input, expected) in [
        (0, -0.006, -0.01),
        (0, 0.004, 0.0),
        (0, 0.006, 0.01),
        (4, -13.374, -13.37),
        (4, -13.376, -13.38),
        (6, 0.504, 0.50),
        (6, 0.506, 0.51),
        (7, 49.499, 49.0),
        (7, 49.5, 50.0),
        (7, 199.5, 200.0),
        (8, -49.501, -50.0),
        (8, -49.5, -49.0),
        (8, -0.5, 0.0),
        (8, 0.5, 1.0),
    ] {
        let spec = parameter_spec(id).expect("spec");
        // Exact plain midpoints below test the tie rule directly.
        // Host normalization may round to either side of a midpoint,
        // so use an unambiguous request infinitesimally above each independent decimal target.
        let host_input = input + 1e-9;
        let normalized = match id {
            0 => (host_input + 24.0) / 36.0,
            4 => (host_input + 60.0) / 72.0,
            6 => host_input * 0.5,
            7 => host_input / 200.0,
            _ => (host_input + 100.0) / 200.0,
        };
        assert!(parameters.set(id, normalized));
        let actual = spec.normalized_to_plain(parameters.get(id).expect("accepted"));
        assert!(
            (actual - expected).abs() < 1e-12,
            "id={id}, input={input}, actual={actual}"
        );
        let parsed = spec.parse(&input.to_string()).expect("numeric input");
        assert!((spec.normalized_to_plain(parsed) - expected).abs() < 1e-12);
        let accepted = parameters.get(id).expect("accepted");
        assert!(parameters.set(id, accepted));
        assert_eq!(parameters.get(id), Some(accepted), "quantization is idempotent");
    }
}

/// Every decimal editing step remains close to its mathematical value and stable after repeated edits.
#[test]
fn complete_plain_grids_are_idempotent_and_preserve_neutral_dsp_band_units() {
    let parameters = Parameters::new();
    for (id, minimum, interval, steps) in [
        (0, -24.0, 0.01, 3600),
        (4, -60.0, 0.01, 7200),
        (6, 0.0, 0.01, 200),
        (7, 0.0, 1.0, 200),
        (8, -100.0, 1.0, 200),
        (9, 0.0, 1.0, 1),
    ] {
        let spec = parameter_spec(id).expect("spec");
        for step in 0..=steps {
            let plain = f64::from(step).mul_add(interval, minimum);
            let normalized = spec.plain_to_normalized(plain);
            assert!((spec.normalized_to_plain(normalized) - plain).abs() < 1e-12);
            assert!(parameters.set(id, normalized));
            assert_eq!(parameters.get(id), Some(normalized), "id={id}, step={step}");
            assert_eq!(spec.snap_normalized(normalized), normalized);
        }
    }
    let shared = Imager::new().shared();
    for band in 0..4 {
        for (field, normalized) in [(0, 5.0 / 6.0), (1, 5.0 / 6.0), (2, 0.5), (3, 0.0), (4, 0.5), (5, 0.0)] {
            let id = band_parameter_id(band, field).expect("band field");
            assert!(parameters.set(id, normalized));
        }
    }
    parameters.publish(&shared);
    for band in 0..4 {
        let values = shared.band_parameters(band).expect("linear float band protocol");
        assert_eq!(
            values.map(f32::to_bits),
            [5.0_f32 / 6.0, 5.0 / 6.0, 0.5, 0.0, 0.5, 0.0].map(f32::to_bits)
        );
    }
}

/// Decades, the geometric midpoint, and equal octave spans define the frequency law independently.
#[test]
fn crossovers_are_exponential_with_double_precision_roundtrips_and_equal_octave_travel() {
    let parameters = Parameters::new();
    for id in 1..=3 {
        let spec = parameter_spec(id).expect("crossover");
        for (normalized, expected) in [
            (0.0, 20.0),
            (1.0 / 3.0, 200.0),
            (0.5, 400_000.0_f64.sqrt()),
            (2.0 / 3.0, 2000.0),
            (1.0, 20_000.0),
        ] {
            assert!((spec.normalized_to_plain(normalized) / expected - 1.0).abs() < 2e-14);
            assert!((spec.plain_to_normalized(expected) - normalized).abs() < 2e-14);
        }
        for step in 0..=1000 {
            let normalized = f64::from(step) / 1000.0;
            let frequency = spec.normalized_to_plain(normalized);
            assert!((spec.plain_to_normalized(frequency) - normalized).abs() < 2e-14);
            assert!(parameters.set(id, normalized));
            assert_eq!(
                parameters.get(id),
                Some(normalized),
                "continuous crossover has no editing grid"
            );
        }
        let precise_frequency = 1234.567_890_123;
        let precise_normalized = spec.parse("1234.567890123 Hz").expect("double-precision frequency");
        assert!((spec.normalized_to_plain(precise_normalized) - precise_frequency).abs() < 1e-10);
        let octave_travel = spec.plain_to_normalized(40.0) - spec.plain_to_normalized(20.0);
        for frequency in [40.0, 100.0, 500.0, 1000.0, 5000.0, 10_000.0] {
            let travel = spec.plain_to_normalized(frequency * 2.0) - spec.plain_to_normalized(frequency);
            assert!((travel - octave_travel).abs() < 2e-14);
        }
        assert_eq!(spec.normalized_to_plain(-1.0), 20.0);
        assert_eq!(spec.normalized_to_plain(2.0), 20_000.0);
        assert_eq!(spec.plain_to_normalized(-1.0), 0.0);
        assert_eq!(spec.plain_to_normalized(30_000.0), 1.0);
    }
}

#[test]
fn finite_text_and_formatting_cover_units_edges_and_direction() {
    for (id, text, expected) in [
        (0, "-0.004 dB", "0.0 dB"),
        (4, "-infinity dB", "-inf dB"),
        (4, "Off", "-inf dB"),
        (4, "-100 dB", "-inf dB"),
        (6, "150%", "1.50"),
        (7, "Off", "0%"),
        (7, "50.5%", "51%"),
        (8, "-35L", "35L"),
        (8, "35% L", "35L"),
        (8, " 35 % r ", "35R"),
        (8, "-35R", "35R"),
        (8, "35%", "35R"),
        (8, "-0.5", "C"),
        (9, "true", "On"),
        (9, "false", "Off"),
        (1, "999.4 Hz", "999 Hz"),
        (1, "999.6 Hz", "1.00 kHz"),
        (1, "1.25 kHz", "1.25 kHz"),
        (1, "12 kHz", "12.0 kHz"),
    ] {
        let spec = parameter_spec(id).expect("spec");
        assert_eq!(
            spec.format(spec.parse(text).expect("valid text")),
            expected,
            "{id}: {text}"
        );
    }
    for (id, text) in [
        (0, "Off"),
        (8, "35%% L"),
        (7, "50%%"),
        (1, "1k"),
        (0, "--1"),
        (0, "+-1"),
        (1, "1e308 kHz"),
    ] {
        assert!(parameter_spec(id).expect("spec").parse(text).is_none(), "{id}: {text}");
    }
    for id in 0..PARAMETER_COUNT as u32 {
        for text in ["", "NaN", "inf", "1e999", "1e-999", "12 junk"] {
            assert!(parameter_spec(id).expect("spec").parse(text).is_none());
        }
    }
}

#[test]
fn editor_binding_reports_the_accepted_grid_value_not_the_request() {
    #[derive(Default)]
    struct Binding(std::cell::Cell<Option<(u32, f64)>>);
    impl EditorBinding for Binding {
        fn begin_edit(&self, _: u32) {}
        fn set_value(&self, id: u32, value: f64) {
            self.0.set(Some((id, value)));
        }
        fn end_edit(&self, _: u32) {}
    }
    let binding = Binding::default();
    let parameters = Parameters::new();
    assert!(binding.publish_value(&parameters, 7, 0.2475));
    assert_eq!(parameters.get(7), Some(0.25));
    assert_eq!(binding.0.get(), Some((7, 0.25)));
    assert!(!binding.publish_value(&parameters, 7, f64::NAN));
    assert!(!binding.publish_value(&parameters, u32::MAX, 0.5));
    assert_eq!(binding.0.get(), Some((7, 0.25)));
}

#[test]
fn invalid_restores_are_transactional_and_missing_fields_are_fixed_defaults() {
    let parameters = Parameters::new();
    assert!(parameters.set(GAIN_PARAMETER, 0.25));
    for value in [f64::NAN, f64::INFINITY, f64::NEG_INFINITY] {
        assert!(!parameters.set(GAIN_PARAMETER, value));
        assert_eq!(
            parameters.get(GAIN_PARAMETER).expect("gain").to_bits(),
            0.25_f64.to_bits()
        );
    }
    assert!(!parameters.set(u32::MAX, 0.5));
    let mut invalid = Snapshot::default();
    invalid.bands[3].recover = f64::NAN;
    assert!(!parameters.restore(invalid));
    assert_eq!(
        parameters.get(GAIN_PARAMETER).expect("gain").to_bits(),
        0.25_f64.to_bits()
    );
    let defaults: Snapshot = serde_json::from_str("{}").expect("fixed defaults");
    assert!(parameters.restore(defaults));
    assert_eq!(
        parameters.get(GAIN_PARAMETER).expect("gain").to_bits(),
        NEUTRAL_GAIN.to_bits()
    );
    assert!(
        serde_json::from_str::<Snapshot>(r#"{"width":0.5}"#).is_err(),
        "unrelated global-width state is not accepted"
    );
    assert!(parameters.set(GAIN_PARAMETER, 2.0));
    assert_eq!(
        parameters.get(GAIN_PARAMETER).expect("gain").to_bits(),
        1.0_f64.to_bits()
    );
}

#[test]
fn publication_is_detached_and_never_changes_unrelated_parameter_atomics() {
    let parameters = Parameters::new();
    let imager = Imager::new();
    let shared = imager.shared();
    parameters.set(GAIN_PARAMETER, 1.0);
    parameters.set(6, 0.0);
    parameters.set(7, 0.8);
    parameters.publish(&shared);
    // Independent 10^(12/20), rounded once to the published f32 domain.
    // Avoid comparing separate platform-dependent powf implementations under Miri.
    let expected = 3.981_071_705_534_972_2_f64 as f32;
    assert!(
        (shared.parameters().gain() - expected).abs() < 1e-6,
        "published gain {} expected {expected}",
        shared.parameters().gain()
    );
    assert_eq!(shared.parameters().width().to_bits(), 1.0_f32.to_bits());
    assert_eq!(shared.crossovers(), [100.0_f32, 500.0, 5000.0]);
    let band = shared.band_parameters(0).expect("band");
    assert_eq!(band[2].to_bits(), 0.0_f32.to_bits());
    assert_eq!(band[3].to_bits(), 0.8_f32.to_bits());
    assert_eq!(
        parameters.get(GAIN_PARAMETER).expect("gain").to_bits(),
        1.0_f64.to_bits()
    );
    assert_eq!(
        parameters.get(12).expect("unrelated width").to_bits(),
        0.5_f64.to_bits()
    );
}

#[test]
fn output_gain_edits_canonicalize_zero_db_and_publish_exact_unity() {
    let parameters = Parameters::new();
    let imager = Imager::new();
    let shared = imager.shared();
    for normalized in [2.0_f64 / 3.0, f64::from(2.0_f32 / 3.0)] {
        assert!(parameters.set(GAIN_PARAMETER, normalized));
        parameters.publish(&shared);
        assert_eq!(shared.parameters().gain().to_bits(), 1.0_f32.to_bits());
        assert_eq!(
            parameters.get(GAIN_PARAMETER).expect("gain").to_bits(),
            NEUTRAL_GAIN.to_bits()
        );
        assert_eq!(parameters.snapshot().gain.to_bits(), NEUTRAL_GAIN.to_bits());

        let spec = parameter_spec(GAIN_PARAMETER).expect("gain");
        assert_eq!(
            spec.normalized_to_plain(parameters.snapshot().gain).to_bits(),
            0.0_f64.to_bits()
        );
    }
}

#[test]
fn output_gain_publication_follows_decibels_without_changing_saved_controls() {
    let parameters = Parameters::new();
    let imager = Imager::new();
    let shared = imager.shared();
    for (decibels, expected_gain) in [
        (-24.0, 0.063_095_734_448_019_33),
        (-12.0, 0.251_188_643_150_958),
        (-6.0, 0.501_187_233_627_272_2),
        (0.0, 1.0),
        (6.0, 1.995_262_314_968_879_5),
        (12.0, 3.981_071_705_534_972_2),
    ] {
        let normalized = (decibels + 24.0) / 36.0;
        assert!(parameters.set(GAIN_PARAMETER, normalized));
        let accepted = parameters.snapshot().gain;
        parameters.publish(&shared);
        assert_eq!(parameters.snapshot().gain, accepted);
        assert!((f64::from(shared.parameters().gain()) / expected_gain - 1.0).abs() < 1e-7);
    }
    // Detached values retain sound precision even when they do not lie on an editor step.
    let normalized = (0.003 + 24.0) / 36.0;
    assert!(parameters.restore(Snapshot {
        gain: normalized,
        ..Snapshot::default()
    }));
    parameters.publish(&shared);
    assert_eq!(parameters.snapshot().gain, normalized);
    assert!((f64::from(shared.parameters().gain()) - 1.000_345_447_327_030_8).abs() < 1e-7);
}

#[test]
fn neutral_host_values_preserve_unprepared_callback_bits_cold_and_after_settling() {
    for normalized in [2.0_f64 / 3.0, f64::from(2.0_f32 / 3.0)] {
        let parameters = Parameters::new();
        let mut imager = Imager::new();
        let shared = imager.shared();
        assert!(parameters.set(GAIN_PARAMETER, normalized));
        parameters.publish(&shared);
        imager.reset();
        for settled in [false, true] {
            if settled {
                assert!(parameters.set(GAIN_PARAMETER, 0.25));
                parameters.publish(&shared);
                imager.reset();
                assert!(parameters.set(GAIN_PARAMETER, normalized));
                parameters.publish(&shared);
                imager.begin_block();
                for _ in 0..960 {
                    let _ = imager.process_frame([0.0; 2]);
                }
            }
            for frame in [
                [-0.0, 0.0],
                [0.0, -0.0],
                [0.1, -0.3],
                [f32::from_bits(0x3f80_0001), -0.25],
            ] {
                assert_eq!(imager.process_frame(frame).map(f32::to_bits), frame.map(f32::to_bits));
            }
        }
    }
}

#[test]
#[cfg_attr(miri, ignore = "full partitioned FIR preparation is verified natively")]
fn neutral_host_values_preserve_prepared_callback_bits_cold_and_after_settling() {
    for float_host in [false, true] {
        let parameters = Parameters::new();
        for id in 0..PARAMETER_COUNT as u32 {
            let normalized = parameter_spec(id).expect("known parameter").default_normalized;
            assert!(parameters.set(
                id,
                if float_host {
                    f64::from(normalized as f32)
                } else {
                    normalized
                }
            ));
        }
        let mut imager = Imager::new();
        imager.set_offline(true);
        let shared = imager.shared();
        parameters.publish(&shared);
        assert!(imager.prepare(48_000.0));
        let latency = imager.latency_samples() as usize;
        assert_eq!(latency, 4352);
        let normalized = parameters.get(GAIN_PARAMETER).expect("gain");
        let frames = [
            [-0.0, 0.0],
            [0.0, -0.0],
            [0.1, -0.3],
            [f32::from_bits(0x3f80_0001), -0.25],
        ];
        for settled in [false, true] {
            if settled {
                assert!(parameters.set(GAIN_PARAMETER, 0.25));
                parameters.publish(&shared);
                imager.begin_block();
                for _ in 0..latency + 960 {
                    let _ = imager.process_frame([0.0; 2]);
                }
                assert!(parameters.set(GAIN_PARAMETER, normalized));
                parameters.publish(&shared);
                imager.begin_block();
                for _ in 0..latency + 960 {
                    let _ = imager.process_frame([0.0; 2]);
                }
            }
            imager.begin_block();
            for index in 0..latency + 1024 {
                let frame = *frames.get(index % frames.len()).expect("wrapped input frame");
                let output = imager.process_frame(frame);
                if index >= latency {
                    let expected = *frames
                        .get((index - latency) % frames.len())
                        .expect("delayed input frame");
                    assert_eq!(
                        output.map(f32::to_bits),
                        expected.map(f32::to_bits),
                        "float_host={float_host}, settled={settled}, frame={index}"
                    );
                }
            }
        }
    }
}

#[test]
fn concurrent_unrelated_writes_are_not_lost() {
    let parameters = Parameters::new();
    std::thread::scope(|scope| {
        for id in 0..PARAMETER_COUNT as u32 {
            let parameters = &parameters;
            scope.spawn(move || {
                for _ in 0..256 {
                    assert!(parameters.set(id, f64::from(id) / 31.0));
                }
            });
        }
    });
    for id in 0..PARAMETER_COUNT as u32 {
        assert_eq!(
            parameters.get(id).expect("known id").to_bits(),
            parameter_spec(id)
                .expect("spec")
                .snap_normalized(f64::from(id) / 31.0)
                .to_bits()
        );
    }
}
