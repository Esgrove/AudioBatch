//! Hardware-free idle-service and accepted-edit delivery contracts using the deterministic CLAP effect.

use std::sync::OnceLock;

use audiobatch_test_clap_plugin::{
    GAIN_PARAMETER, LATENCY_PARAMETER, PLUGIN_ID, PROCESS_FAILURE_PARAMETER, REQUEST_PARAMETER,
};
use clap_host::{InstanceError, Module, PluginInstance, ProcessMode, UiDispatcher, UiThread, service_main_thread};

static DISPATCHER: OnceLock<UiDispatcher> = OnceLock::new();

/// Runs fixture lifecycle work on the sole registered UI thread.
fn on_ui<T: Send + 'static>(work: impl FnOnce(&UiThread) -> T + Send + 'static) -> T {
    DISPATCHER
        .get_or_init(|| UiThread::spawn_dedicated().expect("UI thread"))
        .call(work, &|| false)
        .expect("UI job")
}

/// Creates an inactive in-process plugin without filesystem or installed-plugin access.
fn instance(ui: &UiThread) -> PluginInstance {
    let module = Module::from_entry(
        ui,
        "request-delivery",
        audiobatch_test_clap_plugin::entry().expect("entry"),
    )
    .expect("module");
    PluginInstance::create(ui, &module, PLUGIN_ID.to_str().expect("plugin id"), None).expect("instance")
}

/// Activates a mono instance with a bounded non-power-of-two processing block.
fn prepare(ui: &UiThread, instance: &mut PluginInstance) {
    instance.configure_main_buses(ui, 1).expect("mono ports");
    instance.setup(ui, ProcessMode::Realtime, 48_000.0, 17).expect("setup");
    instance.activate(ui).expect("activation");
}

/// Reads one float32 fixture state parameter by its stable field index.
fn state_value(state: &[u8], index: usize) -> f32 {
    let start = 8 + index * size_of::<f32>();
    f32::from_le_bytes(
        state
            .get(start..start + 4)
            .expect("state parameter")
            .try_into()
            .expect("four bytes"),
    )
}

#[test]
fn paused_lease_defers_native_service_until_retirement() {
    on_ui(|ui| {
        let mut instance = instance(ui);
        prepare(ui, &mut instance);
        let processor = instance.processor(ui).expect("lease");
        instance.set_parameter(ui, GAIN_PARAMETER.get(), 1.5).expect("gain");
        assert!(instance.needs_idle_service());
        assert!(matches!(instance.service_idle(ui), Err(InstanceError::InUse)));
        assert!(matches!(instance.live_state(ui), Err(InstanceError::EditsPending)));
        drop(processor);
        instance.service_idle(ui).expect("idle process delivers required edit");
        assert!(!instance.needs_idle_service());
        assert!((state_value(&instance.state(ui).expect("state"), 0) - 1.5).abs() < f32::EPSILON);
        drop(instance);
        service_main_thread(ui);
    });
}

#[test]
fn requests_published_inside_native_processing_survive_until_next_service() {
    on_ui(|ui| {
        let mut instance = instance(ui);
        prepare(ui, &mut instance);
        instance
            .set_parameter(ui, REQUEST_PARAMETER.get(), 2.0)
            .expect("request process");
        instance.service_idle(ui).expect("deliver request control");
        assert!(
            instance.needs_idle_service(),
            "request raised during process must survive that call"
        );
        instance
            .service_idle(ui)
            .expect("service process request without host edits");
        assert!(!instance.needs_idle_service());
        drop(instance);
        service_main_thread(ui);
    });
}

#[test]
fn an_audio_block_satisfies_old_requests_but_preserves_new_ones() {
    let (instance, mut processor) = on_ui(|ui| {
        let mut instance = instance(ui);
        prepare(ui, &mut instance);
        instance
            .set_parameter(ui, REQUEST_PARAMETER.get(), 2.0)
            .expect("request");
        let processor = instance.processor(ui).expect("lease");
        (instance, processor)
    });
    processor.process(3).expect("first block publishes process request");
    assert!(instance.needs_idle_service());
    processor.process(3).expect("second block satisfies old request");
    assert!(!instance.needs_idle_service());
    drop(processor);
    on_ui(move |ui| {
        drop(instance);
        service_main_thread(ui);
    });
}

#[test]
fn stepped_latency_is_truncated_in_plain_units() {
    on_ui(|ui| {
        let mut instance = instance(ui);
        instance
            .set_parameter(ui, LATENCY_PARAMETER.get(), 5.9)
            .expect("fractional stepped value");
        assert!((state_value(&instance.state(ui).expect("state"), 2) - 5.0).abs() < f32::EPSILON);
        prepare(ui, &mut instance);
        assert_eq!(instance.latency_samples(ui), 5);
        drop(instance);
        service_main_thread(ui);
    });
}

#[test]
fn delivered_edit_snapshots_remain_available_while_a_processor_is_leased() {
    let (instance, mut processor) = on_ui(|ui| {
        let mut instance = instance(ui);
        prepare(ui, &mut instance);
        instance.set_parameter(ui, GAIN_PARAMETER.get(), 0.25).expect("gain");
        let processor = instance.processor(ui).expect("lease");
        (instance, processor)
    });
    processor.process(3).expect("deliver gain");
    on_ui(move |ui| {
        assert!(
            (state_value(&instance.live_state(ui).expect("completed edit snapshot"), 0) - 0.25).abs() < f32::EPSILON
        );
        drop(processor);
        drop(instance);
        service_main_thread(ui);
    });
}

#[test]
fn failed_processing_keeps_edits_pending_until_successful_redelivery() {
    on_ui(|ui| {
        let mut instance = instance(ui);
        prepare(ui, &mut instance);
        instance
            .set_parameter(ui, PROCESS_FAILURE_PARAMETER.get(), 1.0)
            .expect("failure control");
        assert!(matches!(instance.service_idle(ui), Err(InstanceError::Process(_))));
        assert!(instance.needs_idle_service());
        let processor = instance.processor(ui).expect("lease after failed delivery");
        assert!(matches!(instance.live_state(ui), Err(InstanceError::EditsPending)));
        drop(processor);
        instance
            .set_parameter(ui, PROCESS_FAILURE_PARAMETER.get(), 0.0)
            .expect("repair control");
        instance.service_idle(ui).expect("retry old edit before repair edit");
        assert!(!instance.needs_idle_service());
        instance.state(ui).expect("state after complete delivery");
        drop(instance);
        service_main_thread(ui);
    });
}

#[test]
fn deactivation_finishes_failed_delivery_before_replacing_storage() {
    on_ui(|ui| {
        let mut instance = instance(ui);
        prepare(ui, &mut instance);
        instance.set_parameter(ui, GAIN_PARAMETER.get(), 1.5).expect("gain");
        instance
            .set_parameter(ui, PROCESS_FAILURE_PARAMETER.get(), 1.0)
            .expect("failure control");
        assert!(matches!(instance.service_idle(ui), Err(InstanceError::Process(_))));
        instance.deactivate(ui).expect("inactive redelivery");
        assert!((state_value(&instance.state(ui).expect("captured gain"), 0) - 1.5).abs() < f32::EPSILON);
        instance
            .set_parameter(ui, PROCESS_FAILURE_PARAMETER.get(), 0.0)
            .expect("repair");
        instance.flush_parameter_edits(ui).expect("inactive repair");
        prepare(ui, &mut instance);
        assert!(!instance.needs_idle_service());
        drop(instance);
        service_main_thread(ui);
    });
}

#[test]
fn idle_service_preserves_delay_history_when_a_lease_is_reacquired() {
    on_ui(|ui| {
        let mut instance = instance(ui);
        instance
            .set_parameter(ui, LATENCY_PARAMETER.get(), 5.0)
            .expect("latency");
        instance.flush_parameter_edits(ui).expect("inactive edit");
        prepare(ui, &mut instance);
        let mut processor = instance.processor(ui).expect("first lease");
        let mut impulse = [1.0];
        processor.process_in_place(&mut [&mut impulse], 1).expect("impulse");
        drop(processor);
        instance.service_idle(ui).expect("no pending work");
        let mut processor = instance.processor(ui).expect("second lease");
        let mut silence = [0.0; 5];
        processor
            .process_in_place(&mut [&mut silence], 5)
            .expect("continued delay");
        assert_eq!(silence.iter().position(|sample| *sample > 0.5), Some(4));
        drop(processor);
        drop(instance);
        service_main_thread(ui);
    });
}
