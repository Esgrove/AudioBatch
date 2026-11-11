//! CLAP host extensions, bounded edit delivery, and deferred atomic plugin requests.

use std::cell::{Cell, RefCell};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};
use std::time::{Duration, Instant};

use clack_extensions::audio_ports::{AudioPortRescanFlags, HostAudioPorts, HostAudioPortsImpl};
use clack_extensions::audio_ports_config::{HostAudioPortsConfig, HostAudioPortsConfigImpl};
#[cfg(feature = "editor")]
use clack_extensions::gui::HostGui;
use clack_extensions::gui::{GuiSize, HostGuiImpl};
use clack_extensions::latency::{HostLatency, HostLatencyImpl};
use clack_extensions::params::{
    HostParams, HostParamsImplMainThread, HostParamsImplShared, ParamClearFlags, ParamRescanFlags,
};
use clack_extensions::state::{HostState, HostStateImpl};
use clack_extensions::tail::{HostTail, HostTailImpl};
use clack_extensions::thread_check::{HostThreadCheck, HostThreadCheckImpl};
use clack_extensions::timer::{HostTimer, HostTimerImpl, TimerId};
use clack_host::events::UnknownEvent;
use clack_host::events::event_types::ParamValueEvent;
use clack_host::events::io::{OutputEventBuffer, TryPushError};
use clack_host::prelude::*;
use clack_host::utils::ClapId;
use crossbeam_queue::ArrayQueue;

use crate::UiThread;

/// Maximum number of undelivered host automation events.
pub const EDIT_CAPACITY: usize = 2048;
/// Maximum editor dimension accepted from a native plugin.
pub const MAX_EDITOR_DIMENSION: u32 = 16_384;

thread_local! {
    /// Dynamic audio-thread scope, initialized without allocation.
    static AUDIO_SCOPE: Cell<u32> = const { Cell::new(0) };
}

/// Strongest lifecycle operation needed after retiring audio processor handles.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RestartAction {
    /// Stop and reactivate with refreshed ports and compensation.
    Reconfigure,
    /// Destroy and recreate the plugin while preserving its saved state.
    Reload,
}

/// Deferred lifecycle work requested by a plugin, consumed only on the UI thread.
#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
pub struct RestartFlags(pub u32);

impl RestartFlags {
    /// Stop, deactivate, configure and reactivate the instance after retiring its processor.
    pub const RELOAD: u32 = 1;
    /// Latency changed, requiring compensation to be rebuilt.
    pub const LATENCY: u32 = 2;
    /// Audio ports or their configurations changed.
    pub const IO: u32 = 4;
    /// Parameter metadata changed.
    pub const PARAMETERS: u32 = 8;
    /// Tail changed on the audio thread.
    pub const TAIL: u32 = 16;

    /// Returns the strongest requested lifecycle action.
    #[must_use]
    pub const fn action(self) -> Option<RestartAction> {
        if self.0 & Self::RELOAD != 0 {
            Some(RestartAction::Reload)
        } else if self.needs_rebuild() {
            Some(RestartAction::Reconfigure)
        } else {
            None
        }
    }

    /// Returns whether no work was requested.
    #[must_use]
    pub const fn is_empty(self) -> bool {
        self.0 == 0
    }

    /// Returns whether preparation and compensation must be rebuilt.
    #[must_use]
    pub const fn needs_rebuild(self) -> bool {
        self.0 & (Self::RELOAD | Self::LATENCY | Self::IO | Self::TAIL) != 0
    }
}

/// All callbacks callable from realtime threads only publish to fixed storage.
pub struct Shared {
    pub(crate) callback: AtomicBool,
    pub(crate) process: AtomicBool,
    pub(crate) flush: AtomicBool,
    pub(crate) dirty: AtomicBool,
    pub(crate) restart: AtomicU32,
    pub(crate) tail: AtomicU32,
    pub(crate) edits: ArrayQueue<QueuedEdit>,
    pub(crate) accepted_edits: AtomicU64,
    pub(crate) completed_edits: AtomicU64,
    pub(crate) requires_process: AtomicBool,
    pub(crate) gui_open: AtomicBool,
    pub(crate) gui_closed: AtomicBool,
    pub(crate) gui_visibility: AtomicU32,
    pub(crate) gui_resize: AtomicU64,
}

/// One accepted edit, carrying the sequence acknowledged only after its native delivery returns.
pub struct QueuedEdit {
    pub(crate) event: ParamValueEvent,
    pub(crate) sequence: u64,
}

/// Local wrapper allows implementing Clack traits for shared reference-counted state.
pub struct SharedHandlerData(pub(crate) Arc<Shared>);

impl std::ops::Deref for SharedHandlerData {
    type Target = Shared;
    fn deref(&self) -> &Shared {
        &self.0
    }
}

/// Main-thread callbacks and timer registrations, never shared with the callback.
pub struct MainThread {
    pub(crate) shared: Arc<Shared>,
    pub(crate) timers: RefCell<Vec<Timer>>,
    next_timer: Cell<u32>,
}

/// One main-thread timer, with no native callback or worker wakeup.
pub struct Timer {
    pub(crate) id: TimerId,
    pub(crate) period: Duration,
    pub(crate) deadline: Instant,
}

/// Audio-thread host data retained by Clack through activation and deactivation.
pub struct AudioHandler(pub(crate) Arc<Shared>);

/// Ties all CLAP host extension implementations to their correct thread classes.
pub struct Host;

/// Guards the dynamic thread-check audio scope, including lifecycle calls on an idle UI thread.
pub struct AudioScope;

/// Consumes output automation without allocating or retaining arbitrary plugin-owned event data.
pub struct OutputSink<'a>(pub(crate) &'a Shared);

impl Shared {
    /// Allocates bounded request and edit storage before plugin creation.
    pub(crate) fn new() -> Self {
        Self {
            callback: AtomicBool::new(false),
            process: AtomicBool::new(false),
            flush: AtomicBool::new(false),
            dirty: AtomicBool::new(false),
            restart: AtomicU32::new(0),
            tail: AtomicU32::new(0),
            edits: ArrayQueue::new(EDIT_CAPACITY),
            accepted_edits: AtomicU64::new(0),
            completed_edits: AtomicU64::new(0),
            requires_process: AtomicBool::new(false),
            gui_open: AtomicBool::new(false),
            gui_closed: AtomicBool::new(false),
            gui_visibility: AtomicU32::new(0),
            gui_resize: AtomicU64::new(0),
        }
    }
}

impl SharedHandler<'_> for SharedHandlerData {
    fn request_restart(&self) {
        self.restart.fetch_or(RestartFlags::RELOAD, Ordering::Release);
    }
    fn request_process(&self) {
        self.process.store(true, Ordering::Release);
    }
    fn request_callback(&self) {
        self.callback.store(true, Ordering::Release);
    }
}

impl HostParamsImplShared for SharedHandlerData {
    fn request_flush(&self) {
        self.flush.store(true, Ordering::Release);
    }
}

impl HostThreadCheckImpl for SharedHandlerData {
    fn is_main_thread(&self) -> bool {
        UiThread::is_current()
    }
    fn is_audio_thread(&self) -> bool {
        AUDIO_SCOPE.get() != 0
    }
}

impl HostGuiImpl for SharedHandlerData {
    fn resize_hints_changed(&self) {}
    fn request_resize(&self, size: GuiSize) -> Result<(), HostError> {
        if !self.gui_open.load(Ordering::Acquire)
            || size.width == 0
            || size.height == 0
            || size.width > MAX_EDITOR_DIMENSION
            || size.height > MAX_EDITOR_DIMENSION
        {
            return Err(HostError::Message("Editor size is invalid or the editor is not open"));
        }
        self.gui_resize.store(
            (u64::from(size.width) << 32) | u64::from(size.height),
            Ordering::Release,
        );
        Ok(())
    }
    fn request_show(&self) -> Result<(), HostError> {
        if !self.gui_open.load(Ordering::Acquire) {
            return Err(HostError::Message("Open the plugin editor from the plugin chain first"));
        }
        self.gui_visibility.store(1, Ordering::Release);
        Ok(())
    }
    fn request_hide(&self) -> Result<(), HostError> {
        if !self.gui_open.load(Ordering::Acquire) {
            return Err(HostError::Message("Editor is not open"));
        }
        self.gui_visibility.store(2, Ordering::Release);
        Ok(())
    }
    fn closed(&self, _was_destroyed: bool) {
        self.gui_closed.store(true, Ordering::Release);
    }
}

impl MainThread {
    /// Creates main-thread extension storage during plugin instantiation.
    pub(crate) const fn new(shared: Arc<Shared>) -> Self {
        Self {
            shared,
            timers: RefCell::new(Vec::new()),
            next_timer: Cell::new(0),
        }
    }
}
impl MainThreadHandler<'_> for MainThread {}
impl HostStateImpl for MainThread {
    fn mark_dirty(&self) {
        self.shared.dirty.store(true, Ordering::Release);
    }
}
impl HostLatencyImpl for MainThread {
    fn changed(&self) {
        self.shared.restart.fetch_or(RestartFlags::LATENCY, Ordering::Release);
    }
}
impl HostAudioPortsImpl for MainThread {
    fn is_rescan_flag_supported(&self, _flag: AudioPortRescanFlags) -> bool {
        true
    }
    fn rescan(&self, _flags: AudioPortRescanFlags) {
        self.shared.restart.fetch_or(RestartFlags::IO, Ordering::Release);
    }
}
impl HostAudioPortsConfigImpl for MainThread {
    fn rescan(&self) {
        self.shared.restart.fetch_or(RestartFlags::IO, Ordering::Release);
    }
}
impl HostParamsImplMainThread for MainThread {
    fn rescan(&self, flags: ParamRescanFlags) {
        self.shared
            .restart
            .fetch_or(RestartFlags::PARAMETERS, Ordering::Release);
        if flags.contains(ParamRescanFlags::ALL) {
            self.shared.restart.fetch_or(RestartFlags::RELOAD, Ordering::Release);
        }
        if flags.intersects(ParamRescanFlags::VALUES | ParamRescanFlags::ALL) {
            self.shared.dirty.store(true, Ordering::Release);
        }
    }
    fn clear(&self, _param_id: ClapId, _flags: ParamClearFlags) {}
}
impl HostTimerImpl for MainThread {
    fn register_timer(&self, period_ms: u32) -> Result<TimerId, HostError> {
        let mut timers = self.timers.borrow_mut();
        if timers.len() >= 128 {
            return Err(HostError::Message("Too many plugin timers"));
        }
        let id = self.next_timer.get();
        let Some(next) = id.checked_add(1).filter(|value| *value != u32::MAX) else {
            return Err(HostError::Message("Timer identifiers exhausted"));
        };
        self.next_timer.set(next);
        let period = Duration::from_millis(u64::from(period_ms.max(1)));
        let id = TimerId(id);
        timers.push(Timer {
            id,
            period,
            deadline: Instant::now() + period,
        });
        Ok(id)
    }
    fn unregister_timer(&self, id: TimerId) -> Result<(), HostError> {
        let mut timers = self.timers.borrow_mut();
        let previous = timers.len();
        timers.retain(|timer| timer.id != id);
        if timers.len() == previous {
            Err(HostError::Message("Unknown plugin timer"))
        } else {
            Ok(())
        }
    }
}
impl AudioProcessorHandler<'_> for AudioHandler {}
impl HostTailImpl for AudioHandler {
    fn changed(&mut self) {
        self.0.restart.fetch_or(RestartFlags::TAIL, Ordering::Release);
    }
}
impl HostHandlers for Host {
    type Shared<'a> = SharedHandlerData;
    type MainThread<'a> = MainThread;
    type AudioProcessor<'a> = AudioHandler;
    fn declare_extensions(builder: &mut HostExtensions<Self>, _shared: &Self::Shared<'_>) {
        builder
            .register::<HostState>()
            .register::<HostParams>()
            .register::<HostLatency>()
            .register::<HostAudioPorts>()
            .register::<HostAudioPortsConfig>()
            .register::<HostTail>()
            .register::<HostThreadCheck>()
            .register::<HostTimer>();
        #[cfg(feature = "editor")]
        builder.register::<HostGui>();
    }
}
impl AudioScope {
    /// Enters an allocation-free CLAP audio-thread scope.
    pub(crate) fn enter() -> Self {
        AUDIO_SCOPE.set(AUDIO_SCOPE.get() + 1);
        Self
    }
}
impl Drop for AudioScope {
    fn drop(&mut self) {
        AUDIO_SCOPE.set(AUDIO_SCOPE.get() - 1);
    }
}
impl OutputEventBuffer for OutputSink<'_> {
    fn try_push(&mut self, event: &UnknownEvent) -> Result<(), TryPushError> {
        if event.as_event::<ParamValueEvent>().is_some() {
            self.0.dirty.store(true, Ordering::Release);
        }
        Ok(())
    }
}
