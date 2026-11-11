//! In-process production host contracts for all bands, state, neutral bits, latency, and cold callbacks.

use image_control_plugin_rs::{
    BAND_PARAMETER_START, BYPASS_PARAMETER, COMPONENT_ID, CROSSOVER_PARAMETER_START, DELTA_PARAMETER, GAIN_PARAMETER,
    MONO_PARAMETER, PARAMETER_COUNT, factory,
};
use serde_json::Value;
use vst3_host::{ClassId, Module, PluginInstance, PluginState, ProcessMode, ProcessorHandle, UiThread};

/// Independent 48 kHz delay: 4096-sample FIR half-length plus a 256-sample partition.
const LATENCY: usize = 4352;
/// Independent finite ring beyond the latency-compensated endpoint.
const TAIL: u32 = 4096;

/// Groups host cases on one registered libtest UI thread, with explicit instance teardown.
#[test]
#[cfg_attr(
    miri,
    ignore = "full FFT preparation is exercised natively, COM scalar contracts remain Miri-compatible"
)]
fn production_in_process_host_contracts() {
    let ui = UiThread::register_current().expect("host UI thread");
    neutral_bits_and_rate_dependent_reports(&ui);
    every_parameter_survives_state_with_fixed_missing_defaults(&ui);
    malformed_state_is_transactional(&ui);
    display_preferences_roundtrip_without_sound_changes(&ui);
    geometry_roundtrips_across_components_with_fixed_missing_defaults(&ui);
    independent_multiband_automation(&ui);
    cold_callbacks_and_mono_reset(&ui);
}

/// Constructs and prepares the actual plugin wrapper without a native module or hardware device.
fn instance(ui: &UiThread, channels: usize, mode: ProcessMode) -> PluginInstance {
    let module = Module::from_factory(ui, "ImageControl RS", factory());
    let class = module.classes().first().expect("effect class");
    assert_eq!(class.name, "ImageControl RS");
    assert_eq!(class.subcategories, "Fx|Spatial");
    assert_eq!(class.vendor, "Esgrove");
    assert_eq!(class.id, ClassId::from_tuid(&COMPONENT_ID));
    let mut instance = PluginInstance::create(ui, &module, class.id, None).expect("create production component");
    let buses = instance
        .configure_main_buses(ui, channels)
        .expect("matching main buses");
    assert_eq!(buses.inputs, [channels]);
    assert_eq!(buses.outputs, [channels]);
    instance.setup(ui, mode, 48_000.0, 2048).expect("prepare");
    instance.activate(ui).expect("activate");
    instance
}

/// Exercises signed zero and arbitrary samples across irregular in-place callbacks and a complete drain.
fn neutral_bits_and_rate_dependent_reports(ui: &UiThread) {
    let mut instance = instance(ui, 2, ProcessMode::Offline);
    assert_eq!(instance.latency_samples(ui), LATENCY as u32);
    assert_eq!(instance.tail_samples(ui), TAIL);
    let mut processor = instance.processor(ui).expect("processor");
    let signal = [[-0.0, 0.0], [0.123_456_79, -0.987_654_3], [f32::MIN_POSITIVE, 1.0]];
    let frames = LATENCY + 8192;
    let input: Vec<_> = (0..frames)
        .map(|index| {
            if index < 4096 {
                *signal.get(index % signal.len()).expect("signal index")
            } else {
                [0.0; 2]
            }
        })
        .collect();
    let mut position = 0;
    for block in [1, 17, 256, 511, 2048].into_iter().cycle() {
        let count = block.min(frames - position);
        let mut left: Vec<_> = input.iter().skip(position).take(count).map(|frame| frame[0]).collect();
        let mut right: Vec<_> = input.iter().skip(position).take(count).map(|frame| frame[1]).collect();
        processor
            .process_in_place(&mut [&mut left, &mut right], count)
            .expect("in-place callback");
        for (offset, (&left, &right)) in left.iter().zip(&right).enumerate() {
            let index = position + offset;
            let expected = index
                .checked_sub(LATENCY)
                .and_then(|source| input.get(source))
                .copied()
                .unwrap_or([0.0; 2]);
            assert_eq!(
                [left, right].map(f32::to_bits),
                expected.map(f32::to_bits),
                "neutral sample {index}"
            );
        }
        position += count;
        if position == frames {
            break;
        }
    }
    drop(processor);
    instance.deactivate(ui).expect("stop before setup");
    for (rate, latency, tail) in [(96_000.0, 8448, 8192), (44_100.0, 4352, 4096), (48_000.0, 4352, 4096)] {
        instance
            .setup(ui, ProcessMode::Offline, rate, 2048)
            .expect("rate change");
        assert_eq!(instance.latency_samples(ui), latency);
        assert_eq!(instance.tail_samples(ui), tail);
    }
}

/// Decodes state after independently verifying its exact little-endian framing.
fn saved_json(instance: &PluginInstance, ui: &UiThread) -> Value {
    let state = instance.state(ui).expect("save");
    let (length, json) = state.component.split_at(4);
    assert_eq!(
        u32::from_le_bytes(length.try_into().expect("prefix")) as usize,
        json.len()
    );
    serde_json::from_slice(json).expect("JSON")
}

/// Builds explicit state fixtures without using the plugin's serializer.
fn framed(json: &[u8]) -> Vec<u8> {
    let mut bytes = u32::try_from(json.len())
        .expect("bounded fixture")
        .to_le_bytes()
        .to_vec();
    bytes.extend_from_slice(json);
    bytes
}

/// Calls the production host state API with an independently authored JSON payload.
fn restore_json(instance: &mut PluginInstance, ui: &UiThread, json: &[u8]) -> bool {
    instance
        .set_state(
            ui,
            &PluginState {
                component: framed(json),
                controller: Vec::new(),
            },
        )
        .expect("restore result")
}

/// Captures all 31 independently edited controls and verifies defaults do not depend on destination edits.
fn every_parameter_survives_state_with_fixed_missing_defaults(ui: &UiThread) {
    let mut instance = instance(ui, 2, ProcessMode::Offline);
    for id in 0..PARAMETER_COUNT as u32 {
        instance
            .set_parameter(ui, id, f64::from(id + 1) / 32.0)
            .expect("distinct parameter edit");
    }
    for band in 0..4 {
        instance
            .set_parameter(ui, BAND_PARAMETER_START + band * 6 + 3, 1.0)
            .expect("save Recover 200% endpoint");
    }
    let saved = instance.state(ui).expect("all parameter capture");
    assert!(saved.component.len() < 4096);
    instance.deactivate(ui).expect("inactive restore");
    assert!(restore_json(&mut instance, ui, br#"{"version":1,"parameters":{}}"#));
    let defaults = saved_json(&instance, ui);
    assert!(
        defaults.pointer("/parameters/width").is_none(),
        "no global width in state"
    );
    assert_eq!(defaults.get("version").expect("version"), 1);
    assert!(
        (defaults
            .pointer("/parameters/gain")
            .expect("gain")
            .as_f64()
            .expect("gain")
            - 2.0 / 3.0)
            .abs()
            < 1e-15
    );
    for (actual, hertz) in defaults
        .pointer("/parameters/crossovers")
        .expect("crossovers")
        .as_array()
        .expect("crossovers")
        .iter()
        .zip([100.0_f64, 500.0, 5000.0])
    {
        let plain = image_control_plugin_rs::parameter_spec(1)
            .expect("crossover")
            .normalized_to_plain(actual.as_f64().expect("normalized"));
        assert!((plain - hertz).abs() < 0.003);
    }
    for band in defaults
        .pointer("/parameters/bands")
        .expect("bands")
        .as_array()
        .expect("bands")
    {
        for (field, expected) in [
            ("mid", 5.0 / 6.0),
            ("side", 5.0 / 6.0),
            ("width", 0.5),
            ("recover", 0.0),
            ("pan", 0.5),
            ("solo", 0.0),
        ] {
            assert!((band[field].as_f64().expect("band value") - expected).abs() < 1e-15);
        }
    }
    assert!(instance.set_state(ui, &saved).expect("full restore"));
    assert_eq!(instance.state(ui).expect("full recapture"), saved);
    assert!(restore_json(
        &mut instance,
        ui,
        br#"{"version":1,"parameters":{"bands":[{"mid":0.0},{},{},{}]}}"#
    ));
    let partial = saved_json(&instance, ui);
    assert_eq!(partial.pointer("/parameters/bands/0/mid").expect("Mid"), 0.0);
    assert_eq!(partial.pointer("/parameters/bands/0/width").expect("Width"), 0.5);
}

/// Drives the production host codec with independently authored appearance state and retains all 31 sound values.
fn display_preferences_roundtrip_without_sound_changes(ui: &UiThread) {
    let mut instance = instance(ui, 2, ProcessMode::Offline);
    instance.deactivate(ui).expect("stop");
    let before = saved_json(&instance, ui);
    assert!(restore_json(
        &mut instance, ui,
        br#"{"version":1,"parameters":{},"tap":"Pre","display":{"persistence_seconds":1.25,"contrast":0.25,"color_mode":"Phase"}}"#,
    ));
    let restored = saved_json(&instance, ui);
    assert_eq!(restored.get("parameters"), before.get("parameters"));
    assert_eq!(restored.pointer("/tap"), Some(&Value::from("Pre")));
    assert_eq!(
        restored.pointer("/display/persistence_seconds"),
        Some(&Value::from(1.25))
    );
    assert_eq!(restored.pointer("/display/contrast"), Some(&Value::from(0.25)));
    assert_eq!(restored.pointer("/display/color_mode"), Some(&Value::from("Phase")));
    let saved = instance.state(ui).expect("current capture");
    assert!(restore_json(&mut instance, ui, br#"{"version":1,"parameters":{}}"#));
    let defaults = saved_json(&instance, ui);
    assert_eq!(defaults.pointer("/tap"), Some(&Value::from("Post")));
    assert_eq!(defaults.pointer("/display/color_mode"), Some(&Value::from("Frequency")));
    assert!(instance.set_state(ui, &saved).expect("current recall"));
    assert_eq!(instance.state(ui).expect("stable current serialization"), saved);
}

/// Restores logical geometry without an editor, copies it to a fresh component, and checks fixed missing defaults.
fn geometry_roundtrips_across_components_with_fixed_missing_defaults(ui: &UiThread) {
    let mut source = instance(ui, 2, ProcessMode::Offline);
    source.deactivate(ui).expect("stop source");
    assert!(restore_json(
        &mut source,
        ui,
        br#"{"version":1,"parameters":{"gain":0.25},"tap":"Pre","geometry":{"width":1200.5,"height":900.25}}"#,
    ));
    let saved = source.state(ui).expect("geometry capture");
    let mut destination = instance(ui, 2, ProcessMode::Offline);
    destination.deactivate(ui).expect("stop destination");
    let defaults = saved_json(&destination, ui);
    assert_eq!(defaults.pointer("/geometry/width"), Some(&Value::from(1000.0)));
    assert_eq!(defaults.pointer("/geometry/height"), Some(&Value::from(764.0)));
    assert!(destination.set_state(ui, &saved).expect("new component restore"));
    assert_eq!(destination.state(ui).expect("new component recapture"), saved);
    assert!(restore_json(&mut destination, ui, br#"{"version":1,"parameters":{}}"#));
    let reset = saved_json(&destination, ui);
    assert_eq!(reset.get("geometry"), defaults.get("geometry"));
    assert_eq!(source.state(ui).expect("independent source retention"), saved);
}

/// Rejects malformed, incompatible, truncated, oversized, and semantically invalid states without partial writes.
fn malformed_state_is_transactional(ui: &UiThread) {
    let mut instance = instance(ui, 2, ProcessMode::Offline);
    instance.deactivate(ui).expect("stop");
    let original = instance.state(ui).expect("original");
    for component in [
        vec![0, 0, 0],
        vec![0, 0, 0, 0],
        u32::MAX.to_le_bytes().to_vec(),
        framed(br#"{"version":99,"parameters":{}}"#),
        framed(br#"{"version":2,"parameters":{}}"#),
        framed(br#"{"version":1,"parameters":{"gain":0.25},"geometry":{"width":1200,"height":599}}"#),
        framed(br#"{"version":1,"parameters":{"gain":0.25},"geometry":{"width":4097,"height":900}}"#),
        framed(br#"{"version":1,"parameters":{"gain":0.25},"geometry":{"width":1200,"height":null}}"#),
        framed(br#"{"version":1,"parameters":{"gain":0.25},"display":{"persistence_seconds":0.0}}"#),
        framed(br#"{"version":1,"parameters":{"gain":0.25},"display":{"contrast":1.1}}"#),
        framed(br#"{"version":1,"parameters":{"gain":0.25},"display":{"color_mode":"unknown"}}"#),
        framed(br#"{"version":0,"parameters":{}}"#),
        framed(br#"{"version":1,"parameters":{"gain":2.0}}"#),
        framed(br#"{"version":1,"parameters":{"gain":null}}"#),
        framed(br#"{"version":1,"parameters":{"gain":0.25,"crossovers":[0.1,0.2,2.0]}}"#),
        framed(br#"{"version":1,"parameters":{"bands":[{},{},{},{"recover":-1.0}]}}"#),
        framed(br#"{"version":1,"parameters":{"bands":[{},{},{}]}}"#),
        framed(br#"{"version":1,"parameters":{"bands":[{},{},{},{"unknown":0.0}]}}"#),
        framed(br#"{"version":1,"parameters":{"mono":2.0}}"#),
        framed(br#"{"version":1,"parameters":{"width":0.5}}"#),
        framed(br#"{"version":1,"parameters":{"gain":0.5,"gain":0.25}}"#),
        framed(b"not JSON"),
        framed(br#"{"version":1,"parameters":{}} trailing"#),
    ] {
        assert!(
            !instance
                .set_state(
                    ui,
                    &PluginState {
                        component,
                        controller: Vec::new()
                    }
                )
                .expect("codec result")
        );
        assert_eq!(instance.state(ui).expect("unchanged"), original);
    }
}

/// Renders beyond the full FIR delay and control ramps to get independently interpretable constant output.
fn settled_constant(processor: &mut ProcessorHandle, input: [f32; 2]) -> [f32; 2] {
    for (channel, value) in input.into_iter().enumerate() {
        processor.input_channel_mut(channel).expect("input").fill(value);
    }
    for _ in 0..8 {
        processor.process(2048).expect("settled callback");
    }
    std::array::from_fn(|channel| {
        *processor
            .output_channel(channel)
            .expect("output")
            .get(2047)
            .expect("last sample")
    })
}

/// Checks full recovery against a pure-Side sine with independently known mean-square power.
fn fullband_recovery_energy(processor: &mut ProcessorHandle, recover: f64) {
    let mut energy = 0.0;
    for block in 0..8 {
        for channel in 0..2 {
            for (offset, sample) in processor
                .input_channel_mut(channel)
                .expect("input")
                .iter_mut()
                .enumerate()
            {
                let phase = std::f32::consts::TAU * 1000.0 * (block * 2048 + offset) as f32 / 48_000.0;
                *sample = phase.sin() * if channel == 0 { 0.5 } else { -0.5 };
            }
        }
        processor.process(2048).expect("sine callback");
        if block >= 6 {
            for (&left, &right) in processor
                .output_channel(0)
                .expect("left")
                .iter()
                .zip(processor.output_channel(1).expect("right"))
                .take(2048)
            {
                assert!((left - right).abs() < 1e-6);
                energy += f64::from(left).powi(2);
            }
        }
    }
    let expected = 0.125 * recover.powi(2);
    assert!(
        (energy / 4096.0 - expected).abs() < 0.001,
        "known fullband sine recovery power at {}%",
        recover * 100.0
    );
}

/// Delivers real automation queues in both modes and checks analytical band, pan, bypass, gain, Delta, and Mono laws.
fn independent_multiband_automation(ui: &UiThread) {
    for mode in [ProcessMode::Realtime, ProcessMode::Offline] {
        let instance = instance(ui, 2, mode);
        let mut processor = instance.processor(ui).expect("processor");
        for band in 0..4 {
            instance
                .set_parameter(ui, BAND_PARAMETER_START + band * 6, 0.0)
                .expect("Mid Off");
        }
        for sample in settled_constant(&mut processor, [0.5, 0.5]) {
            assert!(sample.abs() < 1e-6);
        }
        for band in 0..4 {
            let base = BAND_PARAMETER_START + band * 6;
            instance.set_parameter(ui, base, 5.0 / 6.0).expect("Mid unity");
            instance.set_parameter(ui, base + 1, 0.0).expect("Side Off");
        }
        for sample in settled_constant(&mut processor, [0.5, -0.5]) {
            assert!(sample.abs() < 1e-6);
        }
        for band in 0..4 {
            let base = BAND_PARAMETER_START + band * 6;
            instance.set_parameter(ui, base + 1, 5.0 / 6.0).expect("Side unity");
            instance.set_parameter(ui, base + 2, 0.0).expect("Width zero");
        }
        for sample in settled_constant(&mut processor, [0.5, -0.5]) {
            assert!(sample.abs() < 1e-6);
        }
        for recover in [1.0, 2.0, 0.0] {
            for band in 0..4 {
                instance
                    .set_parameter(ui, BAND_PARAMETER_START + band * 6 + 3, recover * 0.5)
                    .expect("Recover automation");
            }
            fullband_recovery_energy(&mut processor, recover);
        }
        for band in 0..4 {
            let base = BAND_PARAMETER_START + band * 6;
            instance.set_parameter(ui, base + 2, 0.5).expect("Width unity");
            instance.set_parameter(ui, base + 4, 1.0).expect("Pan right");
            instance.set_parameter(ui, base + 5, 1.0).expect("all Solo");
        }
        let output = settled_constant(&mut processor, [0.5, 0.5]);
        assert!(output[0].abs() < 1e-6);
        assert!((output[1] - 1.0).abs() < 1e-6);
        for (index, frequency) in [80.0_f64, 800.0, 8000.0].into_iter().enumerate() {
            instance
                .set_parameter(
                    ui,
                    CROSSOVER_PARAMETER_START + index as u32,
                    (frequency / 20.0).log(1000.0),
                )
                .expect("crossover queue");
        }
        instance.set_parameter(ui, BYPASS_PARAMETER, 1.0).expect("bypass");
        instance
            .set_parameter(ui, GAIN_PARAMETER, 1.0)
            .expect("saved +12 dB remains bypassed");
        let output = settled_constant(&mut processor, [0.5, -0.25]);
        assert!((output[0] - 0.5).abs() < 1e-6);
        assert!((output[1] + 0.25).abs() < 1e-6);
        assert_eq!(instance.latency_samples(ui), LATENCY as u32);
        instance
            .set_parameter(ui, MONO_PARAMETER, 1.0)
            .expect("Mono after bypass");
        for sample in settled_constant(&mut processor, [0.5, -0.25]) {
            assert!((sample - 0.125).abs() < 1e-6);
        }
        instance.set_parameter(ui, MONO_PARAMETER, 0.0).expect("Mono Off");
        instance.set_parameter(ui, GAIN_PARAMETER, 2.0 / 3.0).expect("zero dB");
        instance.set_parameter(ui, BYPASS_PARAMETER, 0.0).expect("Bypass Off");
        instance.set_parameter(ui, DELTA_PARAMETER, 1.0).expect("Delta");
        let output = settled_constant(&mut processor, [0.5, 0.5]);
        assert!((output[0] + 0.5).abs() < 1e-6);
        assert!((output[1] - 0.5).abs() < 1e-6);
    }
}

/// Runs the first callback on a fresh thread, then verifies persisted +12 dB targets settle before mono output.
fn cold_callbacks_and_mono_reset(ui: &UiThread) {
    let mut instance = instance(ui, 1, ProcessMode::Realtime);
    instance.set_parameter(ui, GAIN_PARAMETER, 1.0).expect("+12 dB");
    let saved = instance.state(ui).expect("flush state");
    instance.deactivate(ui).expect("stop");
    assert!(instance.set_state(ui, &saved).expect("restore"));
    instance.activate(ui).expect("reset settles targets");
    let mut processor = instance.processor(ui).expect("processor");
    processor.input_channel_mut(0).expect("mono").fill(0.25);
    std::thread::scope(|scope| {
        scope
            .spawn(|| {
                for _ in 0..4 {
                    processor.process(2048).expect("cold process");
                }
            })
            .join()
            .expect("fresh callback thread");
    });
    let output = *processor
        .output_channel(0)
        .expect("mono output")
        .get(2047)
        .expect("last sample");
    assert!(0.25_f32.mul_add(-10.0_f32.powf(12.0 / 20.0), output).abs() < 1e-6);
    drop(processor);
    assert_eq!(instance.state(ui).expect("unchanged capture"), saved);
}
