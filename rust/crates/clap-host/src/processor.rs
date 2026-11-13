//! Exclusively leased CLAP processing with UI-retained storage and allocation-free callback errors.

#![expect(
    unsafe_code,
    reason = "the single processor lease exclusively accesses UI-retained audio storage"
)]

use std::cell::Cell;
use std::marker::PhantomData;
use std::sync::Arc;
use std::sync::atomic::Ordering;

use clack_host::events::EventHeader;
use clack_host::events::event_types::{ParamValueEvent, TransportEvent, TransportFlags};
use clack_host::prelude::*;
use clack_host::utils::{BeatTime, ClapId, SecondsTime};
use thiserror::Error;

use crate::host::{AudioScope, EDIT_CAPACITY, OutputSink};
use crate::instance::{BusLayout, InstanceCore, ProcessingSetup, update_tail};

/// Allocation-free process failure. Nonnegative codes denote native failure, negative codes host validation.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Error)]
#[error("The CLAP plugin failed to process audio (code {0})")]
pub struct ProcessError(pub i32);

/// Exclusive access to a prepared CLAP processor, movable between serial audio threads but not shareable.
///
/// Dropping only releases the lease. Native objects and sample buffers remain owned by the UI registry.
pub struct ProcessorHandle {
    core: Arc<InstanceCore>,
    not_sync: PhantomData<Cell<()>>,
}

/// All callback-owned mutable data remains in the UI lifetime anchor, even after callback-side handle drop.
pub struct ProcessorStorage {
    inputs: Vec<Vec<Vec<f32>>>,
    outputs: Vec<Vec<Vec<f32>>>,
    input_ports: AudioPorts,
    output_ports: AudioPorts,
    edits: Box<[ParamValueEvent]>,
    pending_edits: usize,
    pending_sequence: u64,
    layout: BusLayout,
    setup: ProcessingSetup,
    position: i64,
    steady_time: u64,
}

impl ProcessorStorage {
    /// Returns undelivered events after a failed process call to exclusive UI flushing.
    pub(crate) fn append_pending_edits(&mut self, edits: &mut Vec<ParamValueEvent>) -> Option<u64> {
        if self.pending_edits == 0 {
            return None;
        }
        edits.extend(self.edits.iter().take(self.pending_edits).copied());
        self.pending_edits = 0;
        Some(self.pending_sequence)
    }
}

impl ProcessorHandle {
    /// Publishes exclusive access to buffers allocated during activation, preserving the steady clock.
    pub(crate) const fn new(core: Arc<InstanceCore>) -> Self {
        Self {
            core,
            not_sync: PhantomData,
        }
    }

    /// Allocates complete processing storage before activation, never while a renderer holds a lease.
    pub(crate) fn prepare_storage(layout: &BusLayout, setup: ProcessingSetup) -> ProcessorStorage {
        ProcessorStorage {
            inputs: layout
                .inputs
                .iter()
                .map(|count| vec![vec![0.0; setup.max_block_frames]; *count])
                .collect(),
            outputs: layout
                .outputs
                .iter()
                .map(|count| vec![vec![0.0; setup.max_block_frames]; *count])
                .collect(),
            input_ports: AudioPorts::with_capacity(layout.inputs.iter().sum(), layout.inputs.len()),
            output_ports: AudioPorts::with_capacity(layout.outputs.iter().sum(), layout.outputs.len()),
            edits: vec![ParamValueEvent::new(0, ClapId::new(0), Pckn::match_all(), 0.0); EDIT_CAPACITY]
                .into_boxed_slice(),
            pending_edits: 0,
            pending_sequence: 0,
            layout: layout.clone(),
            setup,
            position: 0,
            steady_time: 0,
        }
    }

    /// Returns the main input channel count, excluding sidechains.
    #[must_use]
    pub fn input_channels(&self) -> usize {
        let storage = self.storage();
        storage
            .layout
            .main_input
            .and_then(|index| storage.layout.inputs.get(index).copied())
            .unwrap_or(0)
    }

    /// Returns the main output channel count, excluding auxiliary outputs.
    #[must_use]
    pub fn output_channels(&self) -> usize {
        let storage = self.storage();
        storage
            .layout
            .outputs
            .get(storage.layout.main_output)
            .copied()
            .unwrap_or(0)
    }

    /// Returns the largest accepted process block.
    #[must_use]
    pub fn max_block_frames(&self) -> usize {
        self.storage().setup.max_block_frames
    }

    /// Returns the sample rate selected during activation.
    #[must_use]
    pub fn sample_rate(&self) -> f64 {
        self.storage().setup.sample_rate
    }

    /// Borrows one complete preallocated main input channel.
    pub fn input_channel_mut(&mut self, channel: usize) -> Option<&mut [f32]> {
        let storage = self.storage_mut();
        storage
            .inputs
            .get_mut(storage.layout.main_input?)?
            .get_mut(channel)
            .map(Vec::as_mut_slice)
    }

    /// Borrows one complete preallocated main output channel after processing.
    #[must_use]
    pub fn output_channel(&self, channel: usize) -> Option<&[f32]> {
        let storage = self.storage();
        storage
            .outputs
            .get(storage.layout.main_output)?
            .get(channel)
            .map(Vec::as_slice)
    }

    /// Processes a bounded block without allocation, locks, lifecycle transitions, or host wakeups.
    /// Sidechains receive silence. Oversized blocks are rejected rather than truncated.
    ///
    /// # Errors
    /// Returns `-1` for invalid frame counts or `0` for native CLAP processing failure.
    pub fn process(&mut self, frames: usize) -> Result<(), ProcessError> {
        self.process_block(frames, true)
    }

    /// Advances idle DSP through one silent block without advancing the stopped song transport.
    pub(crate) fn process_silence(&mut self) -> Result<(), ProcessError> {
        let storage = self.storage_mut();
        for port in &mut storage.inputs {
            for channel in port {
                channel.fill(0.0);
            }
        }
        self.process_block(self.max_block_frames(), false)
    }

    /// Processes preallocated buffers and acknowledges edits only after successful native delivery.
    fn process_block(&mut self, frames: usize, playing: bool) -> Result<(), ProcessError> {
        if frames == 0 {
            return Ok(());
        }
        if frames > self.max_block_frames() {
            return Err(ProcessError(-1));
        }
        let shared = &self.core.shared;
        // Consume only requests preceding this operation. Requests raised by native calls survive.
        shared.flush.swap(false, Ordering::AcqRel);
        shared.process.swap(false, Ordering::AcqRel);
        shared.requires_process.swap(false, Ordering::AcqRel);
        let _audio = AudioScope::enter();
        // SAFETY: This non-Sync handle owns the one audio lease, so the UI cannot access either slot.
        let storage = unsafe { (&mut *self.core.storage.get()).as_mut().expect("lease has storage") };
        // SAFETY: Activation created the started processor before the exclusive lease was published.
        let processor = unsafe {
            (&mut *self.core.processor.get())
                .as_mut()
                .expect("lease has active processor")
        };
        for (index, port) in storage.inputs.iter_mut().enumerate() {
            if Some(index) != storage.layout.main_input {
                clear_port_prefix(port, frames);
            }
        }
        for port in &mut storage.outputs {
            clear_port_prefix(port, frames);
        }
        let mut count = storage.pending_edits;
        let mut completed = storage.pending_sequence;
        for event in storage.edits.iter_mut().skip(count) {
            let Some(edit) = shared.edits.pop() else { break };
            *event = edit.event;
            completed = edit.sequence;
            count += 1;
        }
        let input = storage
            .input_ports
            .with_input_buffers(storage.inputs.iter_mut().map(|port| AudioPortBuffer {
                latency: 0,
                channels:
                    AudioPortBufferType::f32_input_only(
                        port.iter_mut().map(|channel| {
                            InputChannel::variable(channel.get_mut(..frames).expect("validated frames"))
                        }),
                    ),
            }));
        let mut output = storage
            .output_ports
            .with_output_buffers(storage.outputs.iter_mut().map(|port| {
                AudioPortBuffer {
                    latency: 0,
                    channels: AudioPortBufferType::f32_output_only(
                        port.iter_mut()
                            .map(|channel| channel.get_mut(..frames).expect("validated frames")),
                    ),
                }
            }));
        let transport = transport(storage.position, storage.setup.sample_rate, playing);
        let mut sink = OutputSink(shared);
        let events = storage.edits.get(..count).expect("bounded edit count");
        let result = processor
            .as_started_mut()
            .map_err(|_| ProcessError(-3))
            .and_then(|started| {
                started
                    .process(
                        &input,
                        &mut output,
                        &InputEvents::from_buffer(&events),
                        &mut OutputEvents::from_buffer(&mut sink),
                        Some(storage.steady_time),
                        Some(&transport),
                    )
                    .map(|_| ())
                    .map_err(|_| ProcessError(0))
            });
        update_tail(shared, &processor.plugin_handle());
        if result.is_ok() {
            if count != 0 {
                shared.completed_edits.store(completed, Ordering::Release);
            }
            storage.pending_edits = 0;
        } else {
            storage.pending_edits = count;
            if count != 0 {
                shared.requires_process.store(true, Ordering::Release);
            }
        }
        storage.pending_sequence = completed;
        if playing {
            storage.position = storage
                .position
                .saturating_add(i64::try_from(frames).unwrap_or(i64::MAX));
        }
        storage.steady_time = storage.steady_time.wrapping_add(frames as u64);
        result
    }

    /// Adapts borrowed main-channel slices through preallocated disjoint CLAP buffers.
    /// Copying avoids illegally aliasing ports for plugins that do not advertise in-place support.
    ///
    /// # Errors
    /// Returns `-2` for mismatched/short channels, or the process failure.
    pub fn process_in_place(&mut self, channels: &mut [&mut [f32]], frames: usize) -> Result<(), ProcessError> {
        if frames > self.max_block_frames() {
            return Err(ProcessError(-1));
        }
        if channels.len() < self.input_channels().max(self.output_channels())
            || channels.iter().any(|channel| channel.len() < frames)
        {
            return Err(ProcessError(-2));
        }
        for (index, source) in channels.iter().enumerate().take(self.input_channels()) {
            if let Some(destination) = self
                .input_channel_mut(index)
                .and_then(|channel| channel.get_mut(..frames))
            {
                destination.copy_from_slice(source.get(..frames).ok_or(ProcessError(-2))?);
            }
        }
        self.process(frames)?;
        for (index, destination) in channels.iter_mut().enumerate().take(self.output_channels()) {
            let source = self
                .output_channel(index)
                .and_then(|channel| channel.get(..frames))
                .ok_or(ProcessError(-2))?;
            destination
                .get_mut(..frames)
                .ok_or(ProcessError(-2))?
                .copy_from_slice(source);
        }
        Ok(())
    }

    /// Clears DSP history on its exclusive audio thread and sets the next transport position.
    pub fn reset(&mut self, position: i64) {
        let _audio = AudioScope::enter();
        // SAFETY: The lease exclusively owns audio operations for this active instance.
        if let Some(processor) = unsafe { &mut *self.core.processor.get() } {
            processor.reset();
        }
        let storage = self.storage_mut();
        storage.position = position;
        storage.steady_time = 0;
    }

    /// Changes song position without resetting the monotonic CLAP steady-time clock or DSP history.
    pub fn set_position(&mut self, position: i64) {
        self.storage_mut().position = position;
    }

    /// Borrows immutable prepared storage under this handle's exclusive lease.
    fn storage(&self) -> &ProcessorStorage {
        // SAFETY: The lease exists for the handle lifetime, excluding UI storage replacement.
        unsafe { (&*self.core.storage.get()).as_ref().expect("lease has storage") }
    }

    /// Borrows mutable prepared storage under this handle's exclusive lease.
    fn storage_mut(&mut self) -> &mut ProcessorStorage {
        // SAFETY: The handle is non-Sync and exclusively leased, so no other access exists.
        unsafe { (&mut *self.core.storage.get()).as_mut().expect("lease has storage") }
    }
}

impl Drop for ProcessorHandle {
    fn drop(&mut self) {
        // No native calls, storage destruction, queue notification, or final Arc release occur here.
        // The UI registry keeps the complete instance and storage alive until its next service tick.
        self.core.leased.store(false, Ordering::Release);
    }
}

/// Zeroes the active `frames` prefix of every channel in a port without touching trailing capacity.
fn clear_port_prefix(port: &mut [Vec<f32>], frames: usize) {
    for channel in port {
        if let Some(samples) = channel.get_mut(..frames) {
            samples.fill(0.0);
        }
    }
}

/// Builds sample-accurate seconds/beat transport using the host's conventional default 120 BPM and 4/4.
fn transport(position: i64, sample_rate: f64, playing: bool) -> TransportEvent {
    let seconds = position as f64 / sample_rate;
    let playing_flag = if playing {
        TransportFlags::IS_PLAYING
    } else {
        TransportFlags::empty()
    };
    TransportEvent {
        header: EventHeader::new_core(0, clack_host::events::EventFlags::empty()),
        flags: playing_flag
            | TransportFlags::HAS_SECONDS_TIMELINE
            | TransportFlags::HAS_BEATS_TIMELINE
            | TransportFlags::HAS_TEMPO
            | TransportFlags::HAS_TIME_SIGNATURE,
        song_pos_beats: BeatTime::from_float(seconds * 2.0),
        song_pos_seconds: SecondsTime::from_float(seconds),
        tempo: 120.0,
        tempo_inc: 0.0,
        loop_start_beats: BeatTime::from_bits(0),
        loop_end_beats: BeatTime::from_bits(0),
        loop_start_seconds: SecondsTime::from_bits(0),
        loop_end_seconds: SecondsTime::from_bits(0),
        bar_start: BeatTime::from_bits(0),
        bar_number: 0,
        time_signature_numerator: 4,
        time_signature_denominator: 4,
    }
}

#[cfg(test)]
mod test_request_delivery {
    use std::sync::OnceLock;

    use audiobatch_test_clap_plugin::PLUGIN_ID;

    use super::*;
    use crate::{InstanceError, Module, PluginInstance, ProcessMode, UiDispatcher, UiThread, service_main_thread};

    static DISPATCHER: OnceLock<UiDispatcher> = OnceLock::new();

    /// Runs each isolated fixture on the one registered lifecycle thread.
    fn on_ui(work: impl FnOnce(&UiThread) + Send + 'static) {
        DISPATCHER
            .get_or_init(|| UiThread::spawn_dedicated().expect("UI thread"))
            .call(work, &|| false)
            .expect("UI job");
    }

    /// Creates an active effect without loading a library or an installed plugin.
    fn instance(ui: &UiThread) -> PluginInstance {
        let module = Module::from_entry(
            ui,
            "clock-contract",
            audiobatch_test_clap_plugin::entry().expect("entry"),
        )
        .expect("module");
        let mut instance =
            PluginInstance::create(ui, &module, PLUGIN_ID.to_str().expect("plugin id"), None).expect("instance");
        instance.configure_main_buses(ui, 1).expect("ports");
        instance.setup(ui, ProcessMode::Realtime, 48_000.0, 17).expect("setup");
        instance.activate(ui).expect("activate");
        instance
    }

    #[test]
    fn reacquiring_a_lease_preserves_the_exact_steady_clock() {
        on_ui(|ui| {
            let instance = instance(ui);
            let mut processor = instance.processor(ui).expect("lease");
            processor.process(7).expect("first process");
            drop(processor);
            let mut processor = instance.processor(ui).expect("new lease");
            assert_eq!(processor.storage().steady_time, 7);
            processor.process(3).expect("second process");
            assert_eq!(processor.storage().steady_time, 10);
            processor.reset(42);
            assert_eq!(processor.storage().steady_time, 0);
            assert_eq!(processor.storage().position, 42);
            drop(processor);
            drop(instance);
            service_main_thread(ui);
        });
    }

    #[test]
    fn idle_processing_advances_only_the_steady_clock() {
        on_ui(|ui| {
            let instance = instance(ui);
            let mut processor = instance.processor(ui).expect("lease");
            processor.set_position(42);
            drop(processor);
            instance.core.shared.process.store(true, Ordering::Release);
            instance.service_idle(ui).expect("idle request");
            let processor = instance.processor(ui).expect("new lease");
            assert_eq!(processor.storage().position, 42);
            assert_eq!(processor.storage().steady_time, 17);
            assert!(
                !transport(42, 48_000.0, false)
                    .flags
                    .contains(TransportFlags::IS_PLAYING)
            );
            drop(processor);
            drop(instance);
            service_main_thread(ui);
        });
    }

    #[test]
    fn editor_flush_without_host_edits_is_serviced_without_processing_audio() {
        on_ui(|ui| {
            let instance = instance(ui);
            instance.core.shared.flush.store(true, Ordering::Release);
            assert!(instance.needs_idle_service());
            instance.service_idle(ui).expect("editor flush");
            assert!(!instance.needs_idle_service());
            let processor = instance.processor(ui).expect("lease");
            assert_eq!(processor.storage().steady_time, 0);
            drop(processor);
            drop(instance);
            service_main_thread(ui);
        });
    }

    #[test]
    fn an_empty_queue_does_not_acknowledge_an_edit_in_flight() {
        on_ui(|ui| {
            let instance = instance(ui);
            let processor = instance.processor(ui).expect("lease");
            instance.core.shared.accepted_edits.store(1, Ordering::Release);
            assert!(instance.core.shared.edits.is_empty());
            assert!(matches!(instance.live_state(ui), Err(InstanceError::EditsPending)));
            instance.core.shared.completed_edits.store(1, Ordering::Release);
            instance.live_state(ui).expect("acknowledged edit permits capture");
            drop(processor);
            drop(instance);
            service_main_thread(ui);
        });
    }
}
