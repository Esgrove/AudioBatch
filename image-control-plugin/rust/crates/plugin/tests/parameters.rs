//! Public parameter contracts independent of the native editor and host framework.

use image_control_dsp::Imager;
use image_control_plugin_rs::params::{
    BAND_CONTROL_COUNT, BAND_PARAMETER_START, BYPASS_PARAMETER, CROSSOVER_PARAMETER_START, DELTA_PARAMETER,
    GAIN_PARAMETER, MONO_PARAMETER, NEUTRAL_GAIN, PARAMETER_COUNT, Parameters, Snapshot, band_parameter_id,
    parameter_spec,
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
        (7, 0.0, 0.0, 100.0),
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
        assert!((spec.normalized_to_plain(spec.default_normalized) - default).abs() < 1e-9);
    }
    for (id, text, plain) in [
        (0, "-12 dB", -12.0),
        (1, "1.5 kHz", 1500.0),
        (7, "25%", 25.0),
        (8, "40% L", -40.0),
        (8, "C", 0.0),
    ] {
        let spec = parameter_spec(id).expect("spec");
        assert!((spec.normalized_to_plain(spec.parse(text).expect("valid text")) - plain).abs() < 1e-9);
    }
    for id in 0..PARAMETER_COUNT as u32 {
        let spec = parameter_spec(id).expect("spec");
        for text in ["NaN", "inf", "-inf", "not a number"] {
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
    assert!((shared.parameters().gain() - 10.0_f32.powf(12.0 / 20.0)).abs() < 1e-6);
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
            (f64::from(id) / 31.0).to_bits()
        );
    }
}
