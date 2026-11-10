//! Native build-tree VST3 contracts with independent delayed audio expectations and stable numeric ids.

use std::path::PathBuf;

use image_control_plugin_rs::{BAND_PARAMETER_START, COMPONENT_ID, PLUGIN_NAME};
use vst3_host::{ClassId, Module, PluginInstance, ProcessMode, UiThread};

/// Loads only an explicitly provided build-tree bundle, never an installed plugin.
#[test]
#[cfg_attr(miri, ignore = "native dynamic module loading is verified outside Miri")]
fn native_bundle_preserves_delayed_neutral_and_band_mute_recall() {
    let Some(bundle) = std::env::var_os("IMAGE_CONTROL_OWN_BUNDLE") else {
        assert!(
            std::env::var_os("CI").is_none(),
            "CI must provide IMAGE_CONTROL_OWN_BUNDLE"
        );
        return;
    };
    let ui = UiThread::register_current().expect("native host UI");
    let module = Module::load(&ui, &PathBuf::from(bundle)).expect("load build-tree VST3");
    let class = module
        .classes()
        .iter()
        .find(|class| class.is_audio_effect())
        .expect("audio effect");
    assert_eq!(class.id, ClassId::from_tuid(&COMPONENT_ID));
    assert_eq!(class.name, PLUGIN_NAME);
    let mut instance = PluginInstance::create(&ui, &module, class.id, None).expect("component and controller");
    instance.configure_main_buses(&ui, 2).expect("stereo buses");
    instance
        .setup(&ui, ProcessMode::Offline, 48_000.0, 512)
        .expect("offline setup");
    instance.activate(&ui).expect("activation");
    assert_eq!(instance.latency_samples(&ui), 4352);
    assert_eq!(instance.tail_samples(&ui), 4096);
    let mut processor = instance.processor(&ui).expect("processor");
    let mut offset = 0;
    let mut found = None;
    for block in 0..12 {
        processor.input_channel_mut(0).expect("left").fill(0.0);
        processor.input_channel_mut(1).expect("right").fill(0.0);
        if block == 0 {
            *processor
                .input_channel_mut(0)
                .expect("left")
                .first_mut()
                .expect("first sample") = 0.75;
            *processor
                .input_channel_mut(1)
                .expect("right")
                .first_mut()
                .expect("first sample") = -0.25;
        }
        processor.process(512).expect("native callback");
        for (index, (&left, &right)) in processor
            .output_channel(0)
            .expect("left")
            .iter()
            .zip(processor.output_channel(1).expect("right"))
            .enumerate()
        {
            if left.abs() > 0.0 || right.abs() > 0.0 {
                assert_eq!(
                    [left.to_bits(), right.to_bits()],
                    [0.75_f32.to_bits(), (-0.25_f32).to_bits()]
                );
                assert!(found.replace(offset + index).is_none(), "one exact delayed impulse");
            }
        }
        offset += 512;
    }
    assert_eq!(found, Some(4352));
    drop(processor);
    for band in 0..4 {
        for field in 0..2 {
            instance
                .set_parameter(&ui, BAND_PARAMETER_START + band * 6 + field, 0.0)
                .expect("mute source");
        }
    }
    let state = instance.state(&ui).expect("save band mute");
    instance.deactivate(&ui).expect("stop before recall");
    assert!(instance.set_state(&ui, &state).expect("restore"));
    instance.activate(&ui).expect("restart with saved targets settled");
    let mut processor = instance.processor(&ui).expect("muted processor");
    for _ in 0..24 {
        processor.input_channel_mut(0).expect("left").fill(0.5);
        processor.input_channel_mut(1).expect("right").fill(-0.25);
        processor.process(512).expect("muted callback");
        for channel in 0..2 {
            assert!(
                processor
                    .output_channel(channel)
                    .expect("output")
                    .iter()
                    .all(|sample| sample.abs() < 1e-6)
            );
        }
    }
}
