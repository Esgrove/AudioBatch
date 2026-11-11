//! Hardware-free CLAP backend contracts through the ordinary safe public API.

use std::sync::OnceLock;

use audiobatch_test_clap_plugin::{
    GAIN_PARAMETER, LATENCY_PARAMETER, PLUGIN_ID, PROCESS_FAILURE_PARAMETER, REQUEST_PARAMETER,
};
use clap_host::{
    InstanceError, Module, PluginInstance, ProcessError, ProcessMode, RestartFlags, UiDispatcher, UiThread,
    service_main_thread,
};

static DISPATCHER: OnceLock<UiDispatcher> = OnceLock::new();

/// Runs a bounded operation on the common lifecycle thread.
fn on_ui<T: Send + 'static>(work: impl FnOnce(&UiThread) -> T + Send + 'static) -> T {
    DISPATCHER
        .get_or_init(|| UiThread::spawn_dedicated().expect("UI thread"))
        .call(work, &|| false)
        .expect("UI job")
}

/// Creates the deterministic effect through its safe Clack entry, without loading a native library.
fn instance(ui: &UiThread) -> PluginInstance {
    let module =
        Module::from_entry(ui, "in-process", audiobatch_test_clap_plugin::entry().expect("entry")).expect("module");
    let class = module.classes(ui).first().expect("descriptor");
    assert_eq!(class.id, PLUGIN_ID.to_str().expect("id"));
    assert!(class.is_effect);
    PluginInstance::create(
        ui,
        &module,
        &class.id,
        Some(DISPATCHER.get().expect("dispatcher").clone()),
    )
    .expect("instance")
}

/// Prepares matching mono ports for non-power-of-two realtime blocks.
fn prepare(ui: &UiThread, instance: &mut PluginInstance) {
    let layout = instance.configure_main_buses(ui, 1).expect("mono configuration");
    assert_eq!(layout.inputs, vec![1]);
    assert_eq!(layout.outputs, vec![1]);
    instance.setup(ui, ProcessMode::Realtime, 48_000.0, 257).expect("setup");
    instance.activate(ui).expect("activation");
}

#[test]
fn inactive_plain_parameter_state_round_trips_and_affects_audio() {
    let (instance, mut processor) = on_ui(|ui| {
        let mut instance = instance(ui);
        instance
            .set_parameter(ui, GAIN_PARAMETER.get(), 1.5)
            .expect("plain gain above normalized one");
        let state = instance.state(ui).expect("inactive params flush");
        assert_ne!(state, [] as [u8; 0]);
        assert!(instance.set_state(ui, &state).expect("load state"));
        prepare(ui, &mut instance);
        let processor = instance.processor(ui).expect("processor");
        (instance, processor)
    });
    let mut channel = [0.5; 17];
    processor.process_in_place(&mut [&mut channel], 17).expect("process");
    assert!(channel.iter().all(|sample| (*sample - 0.75).abs() < f32::EPSILON));
    drop(processor);
    on_ui(move |ui| {
        drop(instance);
        service_main_thread(ui);
    });
}

#[test]
fn leased_processors_exclude_lifecycle_and_can_be_retired_from_another_thread() {
    let (instance, processor) = on_ui(|ui| {
        let mut instance = instance(ui);
        prepare(ui, &mut instance);
        let processor = instance.processor(ui).expect("processor");
        assert!(matches!(instance.processor(ui), Err(InstanceError::InUse)));
        assert!(matches!(instance.deactivate(ui), Err(InstanceError::InUse)));
        assert!(matches!(instance.state(ui), Err(InstanceError::InUse)));
        (instance, processor)
    });
    drop(processor);
    on_ui(move |ui| {
        let mut instance = instance;
        assert!(!instance.has_processor_handles());
        instance.deactivate(ui).expect("retired processor permits deactivate");
        prepare(ui, &mut instance);
        drop(instance.processor(ui).expect("new processor"));
        instance.deactivate(ui).expect("second deactivate");
        drop(instance);
        service_main_thread(ui);
    });
}

#[test]
fn queued_audio_edits_and_atomic_requests_are_serviced_without_dispatch() {
    let (instance, mut processor) = on_ui(|ui| {
        let mut instance = instance(ui);
        prepare(ui, &mut instance);
        let processor = instance.processor(ui).expect("processor");
        instance.set_parameter(ui, GAIN_PARAMETER.get(), 0.25).expect("gain");
        instance
            .set_parameter(ui, REQUEST_PARAMETER.get(), 7.0)
            .expect("requests");
        assert!(matches!(instance.live_state(ui), Err(InstanceError::EditsPending)));
        (instance, processor)
    });
    let mut channel = [1.0; 9];
    processor
        .process_in_place(&mut [&mut channel], 9)
        .expect("process edits");
    assert!(channel.iter().all(|sample| (*sample - 0.25).abs() < f32::EPSILON));
    drop(processor);
    on_ui(move |ui| {
        assert!(instance.take_process_requested());
        assert_eq!(
            instance.take_restart_flags().0 & RestartFlags::RELOAD,
            RestartFlags::RELOAD
        );
        assert!(instance.take_dirty());
        service_main_thread(ui);
        assert_ne!(instance.live_state(ui).expect("delivered state"), [] as [u8; 0]);
        drop(instance);
        service_main_thread(ui);
    });
}

#[test]
fn reset_rewinds_delay_and_validation_errors_remain_small() {
    assert_eq!(size_of::<ProcessError>(), size_of::<i32>());
    let (instance, mut processor) = on_ui(|ui| {
        let mut instance = instance(ui);
        instance.set_parameter(ui, LATENCY_PARAMETER.get(), 5.0).expect("delay");
        instance.flush_parameter_edits(ui).expect("inactive flush");
        prepare(ui, &mut instance);
        assert_eq!(instance.latency_samples(ui), 5);
        assert_eq!(instance.tail_samples(ui), 0);
        let processor = instance.processor(ui).expect("processor");
        (instance, processor)
    });
    let mut impulse = [0.0; 17];
    *impulse.first_mut().expect("sample") = 1.0;
    processor
        .process_in_place(&mut [&mut impulse], 17)
        .expect("delay process");
    assert_eq!(impulse.iter().position(|sample| *sample > 0.5), Some(5));
    processor.reset(100);
    let mut silence = [0.0; 17];
    processor
        .process_in_place(&mut [&mut silence], 17)
        .expect("reset process");
    assert!(silence.iter().all(|sample| sample.abs() < f32::EPSILON));
    assert_eq!(processor.process(258), Err(ProcessError(-1)));
    assert_eq!(
        processor.process_in_place(&mut [&mut silence], 18),
        Err(ProcessError(-2))
    );
    drop(processor);
    on_ui(move |ui| {
        drop(instance);
        service_main_thread(ui);
    });
}

#[test]
fn process_failure_does_not_prevent_control_thread_retirement() {
    let (instance, mut processor) = on_ui(|ui| {
        let mut instance = instance(ui);
        instance
            .set_parameter(ui, PROCESS_FAILURE_PARAMETER.get(), 1.0)
            .expect("failure control");
        instance.flush_parameter_edits(ui).expect("flush");
        prepare(ui, &mut instance);
        let processor = instance.processor(ui).expect("processor");
        (instance, processor)
    });
    assert_eq!(processor.process(7), Err(ProcessError(0)));
    drop(processor);
    on_ui(move |ui| {
        let mut instance = instance;
        instance.deactivate(ui).expect("deactivate failed processor");
        drop(instance);
        service_main_thread(ui);
    });
}

#[test]
fn setup_and_state_validation_leave_an_instance_reusable() {
    on_ui(|ui| {
        let mut instance = instance(ui);
        assert!(matches!(
            instance.setup(ui, ProcessMode::Offline, f64::NAN, 257),
            Err(InstanceError::InvalidSetup { .. })
        ));
        assert!(matches!(
            instance.setup(ui, ProcessMode::Offline, 48_000.0, 0),
            Err(InstanceError::InvalidSetup { .. })
        ));
        assert!(matches!(
            instance.set_parameter(ui, GAIN_PARAMETER.get(), f64::INFINITY),
            Err(InstanceError::NonFiniteParameter { .. })
        ));
        assert!(
            !instance
                .set_state(ui, b"invalid saved state")
                .expect("plugin rejection")
        );
        prepare(ui, &mut instance);
        drop(instance.processor(ui).expect("processor after rejections"));
        drop(instance);
        service_main_thread(ui);
    });
}

#[test]
fn saved_state_wins_over_older_queued_host_edits() {
    on_ui(|ui| {
        let mut instance = instance(ui);
        instance
            .set_parameter(ui, GAIN_PARAMETER.get(), 1.5)
            .expect("saved gain");
        let state = instance.state(ui).expect("snapshot");
        instance
            .set_parameter(ui, GAIN_PARAMETER.get(), 0.25)
            .expect("older queued edit");
        assert!(instance.set_state(ui, &state).expect("restore snapshot"));
        prepare(ui, &mut instance);
        let mut processor = instance.processor(ui).expect("processor");
        let mut channel = [0.5; 3];
        processor
            .process_in_place(&mut [&mut channel], 3)
            .expect("restored audio");
        assert!(channel.iter().all(|sample| (*sample - 0.75).abs() < f32::EPSILON));
        drop(processor);
        drop(instance);
        service_main_thread(ui);
    });
}

#[cfg_attr(miri, ignore = "native dynamic library loading is outside Miri")]
#[test]
fn bundle_loader_resolves_nonhardcoded_executable_names() {
    let directory = tempfile::tempdir().expect("temporary bundle directory");
    let bundle = audiobatch_test_clap_plugin::create_bundle(directory.path(), "ArbitraryPayloadName")
        .expect("build the CLAP fixture example before this test");
    on_ui(move |ui| {
        let module = Module::load(ui, &bundle).expect("load Info.plist-selected payload");
        assert_eq!(
            module.classes(ui).first().expect("class").id,
            PLUGIN_ID.to_str().expect("id")
        );
        let instance =
            PluginInstance::create(ui, &module, PLUGIN_ID.to_str().expect("id"), None).expect("native instance");
        drop(instance);
        service_main_thread(ui);
    });
}

#[cfg(feature = "editor")]
#[test]
fn a_headless_effect_returns_an_actionable_missing_editor_error() {
    on_ui(|ui| {
        let instance = instance(ui);
        let result = clap_host::EditorWindow::open(ui, &instance, "No GUI fixture", None, None);
        assert!(matches!(result, Err(clap_host::EditorError::MissingEditor)));
        drop(instance);
        service_main_thread(ui);
    });
}
