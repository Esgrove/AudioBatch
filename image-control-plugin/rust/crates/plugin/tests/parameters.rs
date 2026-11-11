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
        assert!((spec.normalized_to_plain(0.0) - minimum).abs() < 1e-12);
        assert!((spec.normalized_to_plain(spec.default_normalized) - default).abs() < 1e-12);
        assert!((spec.normalized_to_plain(1.0) - maximum).abs() < 1e-12);
    }
    for (index, default) in [100.0, 500.0, 5000.0].into_iter().enumerate() {
        let spec = parameter_spec(CROSSOVER_PARAMETER_START + index as u32).expect("crossover");
        assert!((spec.normalized_to_plain(spec.default_normalized) - default).abs() < 0.003);
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
        assert!((spec.normalized_to_plain(spec.parse(text).expect("valid text")) - plain).abs() < 0.003);
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

/// Expected plain grid points are independent decimal targets, including asymmetric negative ties.
#[test]
fn native_plain_grid_boundaries_apply_to_host_edits_and_text() {
    let parameters = Parameters::new();
    for (id, input, expected, tolerance) in [
        (0, -0.006, -0.01, 0.000_004),
        (0, 0.004, 0.0, 0.000_004),
        (0, 0.006, 0.01, 0.000_004),
        (4, -13.374, -13.37, 0.000_008),
        (4, -13.376, -13.38, 0.000_008),
        (6, 0.504, 0.50, 0.000_001),
        (6, 0.506, 0.51, 0.000_001),
        (7, 49.499, 49.0, 0.0),
        (7, 49.5, 50.0, 0.0),
        (7, 199.5, 200.0, 0.0),
        (8, -49.501, -50.0, 0.0),
        (8, -49.5, -49.0, 0.0),
        (8, -0.5, 0.0, 0.0),
        (8, 0.5, 1.0, 0.0),
    ] {
        let spec = parameter_spec(id).expect("spec");
        let normalized = match id {
            0 => (input + 24.0) / 36.0,
            4 => (input + 60.0) / 72.0,
            6 => input * 0.5,
            7 => input / 200.0,
            _ => (input + 100.0) / 200.0,
        };
        assert!(parameters.set(id, normalized));
        let actual = spec.normalized_to_plain(parameters.get(id).expect("accepted"));
        assert!(
            (actual - expected).abs() <= tolerance,
            "id={id}, input={input}, actual={actual}"
        );
        let parsed = spec.parse(&input.to_string()).expect("numeric input");
        assert!((spec.normalized_to_plain(parsed) - expected).abs() <= tolerance);
        let accepted = parameters.get(id).expect("accepted");
        assert!(parameters.set(id, accepted));
        assert_eq!(parameters.get(id), Some(accepted), "quantization is idempotent");
    }
}

/// Every declared native step remains stable after repeated host publication and plain conversion.
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

/// Native reference values come from the float `NormalisableRange` expression, not RS conversion output.
#[test]
fn crossover_uses_native_float_power_skew_not_an_exponential_mapping() {
    let spec = parameter_spec(1).expect("crossover");
    for (normalized, expected) in [
        (0.0, 20.0),
        (0.1, 20.187_408_447_265_625),
        (0.25, 38.773_849_487_304_69),
        (0.5, 632.455_322_265_625),
        (0.75, 4723.568_359_375),
        (0.9, 11_783.472_656_25),
        (1.0, 20_000.0),
    ] {
        assert!((spec.normalized_to_plain(normalized) - expected).abs() < 0.002);
        let roundtrip = spec.plain_to_normalized(expected);
        assert!((roundtrip - normalized).abs() < 0.000_002);
    }
    assert_eq!(spec.normalized_to_plain(-1.0), 20.0);
    assert_eq!(spec.normalized_to_plain(2.0), 20_000.0);
    assert_eq!(spec.plain_to_normalized(-1.0), 0.0);
    assert_eq!(spec.plain_to_normalized(30_000.0), 1.0);
}

#[test]
fn native_text_grammar_and_formatting_cover_units_edges_and_direction() {
    for (id, text, expected) in [
        (0, "-0.004 dB", "0.0 dB"),
        (4, "-infinity dB", "-inf dB"),
        (4, "-100 dB", "-inf dB"),
        (6, "150%", "1.50"),
        (7, "Off", "0%"),
        (7, "50.5%", "51%"),
        (8, "-35L", "35L"),
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
    for (id, text) in [(4, "Off"), (8, "35% L"), (7, "50%%"), (1, "1k"), (0, "--1"), (0, "+-1")] {
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
fn output_gain_edits_canonicalize_neutral_but_recall_keeps_legacy_neutral_bits() {
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

        assert!(parameters.restore(Snapshot {
            gain: normalized,
            ..Snapshot::default()
        }));
        parameters.publish(&shared);
        assert_eq!(shared.parameters().gain().to_bits(), 1.0_f32.to_bits());
        assert_eq!(parameters.snapshot().gain.to_bits(), normalized.to_bits());
    }
}

#[test]
fn detached_recall_retains_legacy_output_gain_neighbors_without_snapping() {
    let parameters = Parameters::new();
    let imager = Imager::new();
    let shared = imager.shared();
    let double_neutral = 2.0_f64 / 3.0;
    let float_neutral = 2.0_f32 / 3.0;
    for (normalized, expected_ordering) in [
        // These genuine double neighbors remain distinct in state, but round to unity in DSP's f32 domain.
        (double_neutral.next_down(), std::cmp::Ordering::Equal),
        (double_neutral.next_up(), std::cmp::Ordering::Equal),
        // Even the immediate f64 neighbors of the widened float neutral must use the ordinary gain mapping.
        (f64::from(float_neutral).next_down(), std::cmp::Ordering::Greater),
        (f64::from(float_neutral).next_up(), std::cmp::Ordering::Greater),
        (f64::from(float_neutral.next_down()), std::cmp::Ordering::Less),
        (f64::from(float_neutral.next_up()), std::cmp::Ordering::Greater),
    ] {
        assert!(parameters.restore(Snapshot {
            gain: normalized,
            ..Snapshot::default()
        }));
        parameters.publish(&shared);
        assert_eq!(parameters.snapshot().gain.to_bits(), normalized.to_bits());
        assert_eq!(
            shared.parameters().gain().total_cmp(&1.0),
            expected_ordering,
            "normalized bits {:016x}",
            normalized.to_bits()
        );
    }
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
