//! CLAP GUI extension reusing the native Iced child and bounded UI-to-host gesture transport.
//!
//! Only the main thread owns native windows and calls host GUI/state callbacks.
//! Audio callbacks drain copyable, preallocated events without accessing a native window or binding.

#![expect(
    unsafe_code,
    reason = "native parent attachment and closed-before-drop host-handle lifetime boundary"
)]

use std::cell::{Cell, RefCell};
use std::rc::{Rc, Weak};
use std::sync::Arc;

use clack_extensions::gui::{
    AspectRatioStrategy, GuiApiType, GuiConfiguration, GuiResizeHints, GuiSize, HostGui, PluginGuiImpl, Window,
};
use clack_extensions::params::HostParams;
use clack_extensions::state::HostState;
use clack_plugin::events::event_types::{ParamGestureBeginEvent, ParamGestureEndEvent, ParamValueEvent};
use clack_plugin::prelude::*;
use crossbeam_queue::ArrayQueue;

use super::{MainThread, Shared};
use crate::deferred_ui::Deferred;
use crate::editor_geometry::{EditorGeometry, LogicalSize, MAX_EDITOR_SIZE, MIN_EDITOR_SIZE};
use crate::iced_bridge::Editor;
use crate::params::{EditorBinding, PARAMETER_COUNT, Parameters, parameter_spec};

/// Fixed event storage leaves one end event for every simultaneously open parameter gesture.
const EDIT_CAPACITY: usize = 1024;
/// Maximum event drain per callback, including one retry from the previous host rejection.
const MAXIMUM_OUTPUT_EVENTS: usize = EDIT_CAPACITY + 1;

/// Copyable host notification, whose lifetime never owns editor or DSP resources.
#[derive(Clone, Copy)]
enum Edit {
    Begin(u32),
    Value(u32, f64, u64),
    End(u32),
}

/// Independent bounded queues keep a rejected front event ahead of later gesture events.
pub(super) struct EditQueue {
    /// UI produces, CLAP's mutually exclusive process/flush consumer removes.
    queue: ArrayQueue<Edit>,
    /// A host-rejected event is retried before consuming any later event.
    retry: ArrayQueue<Edit>,
}

impl EditQueue {
    /// Allocates all transport capacity before audio activation.
    pub(super) fn new() -> Self {
        Self {
            queue: ArrayQueue::new(EDIT_CAPACITY),
            retry: ArrayQueue::new(1),
        }
    }

    /// Drains only a bounded prefix and retains the first rejected event without reordering.
    pub(super) fn flush(&self, output: &mut OutputEvents, parameters: &Parameters) {
        for _ in 0..MAXIMUM_OUTPUT_EVENTS {
            let Some(edit) = self.retry.pop().or_else(|| self.queue.pop()) else {
                break;
            };
            let result = match edit {
                Edit::Begin(id) => output.try_push(ParamGestureBeginEvent::new(0, ClapId::new(id))),
                Edit::Value(id, value, generation) => {
                    if generation != parameters.restore_generation() {
                        continue;
                    }
                    output.try_push(ParamValueEvent::new(0, ClapId::new(id), Pckn::match_all(), value))
                }
                Edit::End(id) => output.try_push(ParamGestureEndEvent::new(0, ClapId::new(id))),
            };
            if result.is_err() {
                // CLAP serializes all process/flush consumers, so this slot is empty after its pop.
                let _ = self.retry.push(edit);
                break;
            }
        }
    }
}

/// Main-thread host binding is invalidated before native editor teardown releases the host lifetime.
struct Binding {
    /// Authoritative atomics are independent of pending host event delivery.
    parameters: Arc<Parameters>,
    /// Queue storage outlives any retained Iced state without retaining a host object.
    edits: Arc<EditQueue>,
    /// Closed before the original CLAP main-thread host lifetime expires.
    host: HostMainThreadHandle<'static>,
    /// Native view lifetime and deferred geometry are retained only weakly by Iced.
    ui: Weak<UiState>,
    /// One bit owns each open host gesture, including interrupted teardown.
    gestures: Cell<u32>,
    /// An old retained Iced program cannot become live when another editor is created.
    open: Cell<bool>,
    /// Creation identity rejects callbacks that span a reentrant destroy/recreate.
    generation: u64,
}

impl Binding {
    /// Requests event delivery only while the owning GUI lifetime is open.
    fn request_flush(&self) {
        let _callback = self.ui.upgrade().map(UiCall::new);
        if let Some(extension) = self.host.get_extension::<HostParams>() {
            extension.request_flush(self.host.as_shared());
        } else {
            self.host.request_process();
        }
    }
}

impl EditorBinding for Binding {
    /// Closed native lifetimes cannot invoke the lifetime-erased host handle.
    fn is_open(&self) -> bool {
        self.open.get()
            && self
                .ui
                .upgrade()
                .is_some_and(|ui| ui.created.get() && ui.generation.get() == self.generation)
    }

    /// Reserves enough queue space to balance every accepted begin even under host backpressure.
    fn begin_edit(&self, id: u32) {
        if !self.is_open() || parameter_spec(id).is_none() || self.gestures.get() & (1 << id) != 0 {
            return;
        }
        if self.edits.queue.len() >= EDIT_CAPACITY - PARAMETER_COUNT {
            return;
        }
        if self.edits.queue.push(Edit::Begin(id)).is_ok() {
            self.gestures.set(self.gestures.get() | (1 << id));
            self.request_flush();
        }
    }

    /// Rejects overload without inventing unreported parameter edits or consuming end-event reserves.
    fn set_value(&self, id: u32, normalized: f64) {
        if !self.is_open()
            || !normalized.is_finite()
            || id as usize >= PARAMETER_COUNT
            || self.gestures.get() & (1 << id) == 0
        {
            return;
        }
        let Some(spec) = parameter_spec(id) else {
            return;
        };
        if self.edits.queue.len() >= EDIT_CAPACITY - PARAMETER_COUNT {
            return;
        }
        let normalized = spec.snap_normalized(normalized);
        if self
            .edits
            .queue
            .push(Edit::Value(
                id,
                spec.normalized_to_plain(normalized),
                self.parameters.restore_generation(),
            ))
            .is_ok()
        {
            self.parameters.set(id, normalized);
            self.request_flush();
        }
    }

    /// Queue admission precedes shared parameter publication, so overload cannot create unreported sound changes.
    fn publish_value(&self, _parameters: &Parameters, id: u32, normalized: f64) -> bool {
        if !self.is_open()
            || !normalized.is_finite()
            || id as usize >= PARAMETER_COUNT
            || self.gestures.get() & (1 << id) == 0
            || self.edits.queue.len() >= EDIT_CAPACITY - PARAMETER_COUNT
        {
            return false;
        }
        self.set_value(id, normalized);
        true
    }

    /// Consumes the reserved slot for exactly one matching end.
    fn end_edit(&self, id: u32) {
        if id as usize >= PARAMETER_COUNT || self.gestures.get() & (1 << id) == 0 {
            return;
        }
        self.gestures.set(self.gestures.get() & !(1 << id));
        let _ = self.edits.queue.push(Edit::End(id));
        if self.is_open() {
            self.request_flush();
        }
    }

    /// Marks persistent display changes through the CLAP state extension rather than sound automation.
    fn mark_dirty(&self) {
        if !self.is_open() {
            return;
        }
        let _callback = self.ui.upgrade().map(UiCall::new);
        if let Some(extension) = self.host.get_extension::<HostState>() {
            extension.mark_dirty(&self.host);
        }
    }

    /// Balances outstanding gestures while the CLAP host is still alive.
    fn close(&self) {
        if !self.open.replace(false) {
            return;
        }
        let had_gestures = self.gestures.get() != 0;
        for id in 0..PARAMETER_COUNT as u32 {
            self.end_edit(id);
        }
        if had_gestures {
            self.request_flush();
        }
    }

    /// Releases all plugin borrows before reentrant host resize requests and defers native geometry.
    fn request_resize(&self, width: u32, height: u32) -> bool {
        let Some(ui) = self.ui.upgrade().filter(|ui| ui.created.get()) else {
            return false;
        };
        let size = GuiSize { width, height };
        if !ui.valid_size(size) {
            return false;
        }
        let Some(extension) = self.host.get_extension::<HostGui>() else {
            return false;
        };
        let generation = ui.generation.get();
        let _callback = UiCall::new(Rc::clone(&ui));
        let accepted = extension.request_resize(self.host.as_shared(), width, height).is_ok();
        if !accepted || !self.is_open() || ui.generation.get() != generation {
            return false;
        }
        ui.size.set(size);
        ui.publish_geometry();
        ui.pending_size.set(Some(size));
        let _ = ui.deferred.schedule();
        self.mark_dirty();
        true
    }
}

/// Main-thread resources and deferred operations never borrow an active Iced program.
struct UiState {
    /// Native storage is cloned before calls which may reenter the host.
    editor: RefCell<Option<Rc<Editor>>>,
    /// Gesture binding is retained independently of the native window.
    binding: RefCell<Option<Rc<Binding>>>,
    /// Explicit create/destroy lifetime, checked after every host callback.
    created: Cell<bool>,
    /// Lifecycle generations cancel work during reentrant close/reopen.
    generation: Cell<u64>,
    /// Authoritative host dimensions in points on macOS and physical pixels on Windows.
    size: Cell<GuiSize>,
    /// Fallback content scale, replaced by the child's actual scale for Windows geometry.
    scale: Cell<f64>,
    /// Coherent geometry shared with detached state callbacks.
    geometry: Arc<EditorGeometry>,
    /// Latest requested native size, coalesced outside Iced's active update.
    pending_size: Cell<Option<GuiSize>>,
    /// Latest requested visibility without destroying native resources.
    pending_visible: Cell<Option<bool>>,
    /// Latest fallback DPI change deferred outside active Iced updates.
    pending_scale: Cell<Option<f64>>,
    /// A nested host run loop must not flush native changes before its Iced callback returns.
    callback_depth: Cell<usize>,
    /// Native callbacks cannot recursively enter an active geometry flush.
    flushing: Cell<bool>,
    /// Schedules native operations outside the host callback and active Iced update.
    deferred: Deferred,
}

impl UiState {
    /// Uses logical points on macOS and actual native DPI on Windows exactly once.
    fn native_scale(&self) -> f64 {
        if cfg!(windows) {
            let editor = self.editor.borrow().clone();
            editor
                .and_then(|editor| editor.scale_factor().ok())
                .unwrap_or_else(|| self.scale.get())
        } else {
            1.0
        }
    }

    /// Requires complete geometry to remain within the editor's logical bounds.
    fn valid_size(&self, size: GuiSize) -> bool {
        let scale = self.native_scale();
        let dimensions = (size.width, size.height);
        let minimum = (LogicalSize {
            width: f64::from(MIN_EDITOR_SIZE.0),
            height: f64::from(MIN_EDITOR_SIZE.1),
        })
        .native(scale);
        let maximum = (LogicalSize {
            width: f64::from(MAX_EDITOR_SIZE.0),
            height: f64::from(MAX_EDITOR_SIZE.1),
        })
        .native(scale);
        (minimum.0 as u32..=maximum.0 as u32).contains(&dimensions.0)
            && (minimum.1 as u32..=maximum.1 as u32).contains(&dimensions.1)
    }

    /// Captures accepted host geometry independently of any asynchronously resized child.
    fn publish_geometry(&self) {
        let size = self.size.get();
        self.geometry.publish(self.geometry.snapshot().acknowledged(
            size.width.cast_signed(),
            size.height.cast_signed(),
            self.native_scale(),
        ));
    }

    /// Applies coalesced native operations only after the binding callback and Iced update return.
    fn flush(&self) {
        if !self.created.get() {
            return;
        }
        if self.callback_depth.get() != 0 || self.flushing.get() {
            let _ = self.deferred.schedule();
            return;
        }
        self.flushing.set(true);
        let editor = self.editor.borrow().clone();
        if let Some(editor) = editor {
            if let Some(scale) = self.pending_scale.take() {
                let _ = editor.set_scale_factor(scale);
            }
            if self.created.get()
                && let Some(size) = self.pending_size.take()
            {
                let _ = editor.resize(size.width, size.height);
            }
            if self.created.get()
                && let Some(visible) = self.pending_visible.take()
            {
                let _ = editor.set_visible(visible);
            }
        }
        self.flushing.set(false);
    }

    /// Invalidates callbacks before synchronously closing all worker and native-child ownership.
    fn destroy(&self) {
        self.created.set(false);
        self.generation.set(self.generation.get().wrapping_add(1));
        self.deferred.cancel();
        self.pending_size.set(None);
        self.pending_visible.set(None);
        self.pending_scale.set(None);
        // Detach both old owners before any request_flush or native callback can recreate the GUI.
        let binding = self.binding.borrow_mut().take();
        let editor = self.editor.borrow_mut().take();
        if let Some(binding) = binding {
            binding.close();
        }
        if let Some(editor) = editor {
            // A CLAP destroy callback has no retry result.
            // The existing bridge's Win32 reentrant removal limitation still applies:
            // hosts must destroy on an unborrowed native UI turn.
            let _ = editor.close();
        }
    }
}

/// Keeps nested native host event loops from flushing inside a surrounding Iced update.
struct UiCall(Rc<UiState>);

impl UiCall {
    /// Enters callback depth without retaining a `RefCell` or native window borrow.
    fn new(ui: Rc<UiState>) -> Self {
        ui.callback_depth.set(ui.callback_depth.get() + 1);
        Self(ui)
    }
}

impl Drop for UiCall {
    /// A pending timer runs only after the outermost synchronous callback returns.
    fn drop(&mut self) {
        self.0.callback_depth.set(self.0.callback_depth.get() - 1);
    }
}

/// Borrowed instance/host references remain lifetime-checked except inside the explicitly closed UI binding.
pub(super) struct Gui<'a> {
    /// Clack's host-main-thread ownership lasts until this adapter is destroyed.
    host: HostMainThreadHandle<'a>,
    /// Native state is UI-only and has no strong cycle with the embedded program.
    ui: Rc<UiState>,
}

impl<'a> Gui<'a> {
    /// Allocates idle UI state without opening a native window or registering a timer.
    pub(super) fn new(host: HostMainThreadHandle<'a>, shared: &'a Shared) -> Self {
        let logical = shared.instance.geometry.snapshot();
        let ui = Rc::new_cyclic(|weak: &Weak<UiState>| {
            let weak = weak.clone();
            UiState {
                editor: RefCell::new(None),
                binding: RefCell::new(None),
                created: Cell::new(false),
                generation: Cell::new(0),
                size: Cell::new(GuiSize {
                    width: logical.width.round() as u32,
                    height: logical.height.round() as u32,
                }),
                scale: Cell::new(1.0),
                geometry: Arc::clone(&shared.instance.geometry),
                pending_size: Cell::new(None),
                pending_visible: Cell::new(None),
                pending_scale: Cell::new(None),
                callback_depth: Cell::new(0),
                flushing: Cell::new(false),
                deferred: Deferred::new(move || {
                    if let Some(ui) = weak.upgrade() {
                        ui.flush();
                    }
                }),
            }
        });
        Self { host, ui }
    }
}

impl Drop for Gui<'_> {
    /// Closes the binding before the original host lifetime can expire.
    fn drop(&mut self) {
        self.ui.destroy();
    }
}

/// Stable public layout of `clap_wrapper_host_information_t` from the pinned wrapper header.
#[repr(C)]
struct WrapperInformation {
    /// Returns a static wrapper flavor string for this host lifetime.
    flavor: Option<unsafe extern "C" fn(*const std::ffi::c_void) -> *const std::ffi::c_char>,
    /// Returns the underlying host name, which this adapter never invokes.
    underlying_name: Option<unsafe extern "C" fn(*const std::ffi::c_void) -> *const std::ffi::c_char>,
}

/// Identifies the wrapper's documented extension instead of guessing from the DAW name.
fn is_auv2_wrapper(host: HostMainThreadHandle<'_>) -> bool {
    let raw = host.as_raw();
    let Some(get_extension) = raw.get_extension else {
        return false;
    };
    // SAFETY: CLAP keeps this host and its extension callback alive for the current call.
    let information = unsafe { get_extension(raw, c"clap-wrapper.host-information/0".as_ptr()) };
    if information.is_null() {
        return false;
    }
    // SAFETY: This identifier specifies the pinned public two-function extension layout.
    let information = unsafe { &*information.cast::<WrapperInformation>() };
    let Some(flavor) = information.flavor else {
        return false;
    };
    // SAFETY: This thread-safe extension returns a host-lifetime C string or null.
    let flavor = unsafe { flavor(std::ptr::from_ref(raw).cast()) };
    if flavor.is_null() {
        return false;
    }
    // SAFETY: The wrapper guarantees a terminated static flavor string for its host lifetime.
    (unsafe { std::ffi::CStr::from_ptr(flavor) }) == c"auv2"
}

/// Returns only supported native embedding APIs, never an unrelated standalone application window.
const fn native_api() -> GuiApiType<'static> {
    if cfg!(target_os = "macos") {
        GuiApiType::COCOA
    } else {
        GuiApiType::WIN32
    }
}

impl PluginGuiImpl for MainThread<'_> {
    /// Supports native embedded Cocoa/Win32 children only.
    fn is_api_supported(&self, configuration: GuiConfiguration) -> bool {
        !configuration.is_floating && configuration.api_type == native_api()
    }

    /// Advertises the native embedding API without a floating fallback.
    fn get_preferred_api(&self) -> Option<GuiConfiguration<'_>> {
        Some(GuiConfiguration {
            api_type: native_api(),
            is_floating: false,
        })
    }

    /// Creates an idle lifetime, with expensive native/GPU attachment deferred until the host parent is supplied.
    fn create(&self, configuration: GuiConfiguration) -> Result<(), PluginError> {
        if !self.is_api_supported(configuration) || self.gui.ui.created.replace(true) {
            return Err(PluginError::Message("Unsupported or duplicate CLAP GUI creation"));
        }
        let geometry = self.shared.instance.geometry.snapshot();
        let (width, height) = geometry.native(self.gui.ui.native_scale());
        self.gui.ui.size.set(GuiSize {
            width: width as u32,
            height: height as u32,
        });
        // SAFETY: Gui::drop invalidates and closes every binding before the original host expires.
        // Surviving Iced state only retains a closed binding, whose host callbacks all check is_open.
        let host = unsafe { self.gui.host.with_arbitrary_lifetime() };
        let binding = Rc::new(Binding {
            parameters: Arc::clone(&self.shared.instance.parameters),
            edits: Arc::clone(&self.shared.edits),
            host,
            ui: Rc::downgrade(&self.gui.ui),
            gestures: Cell::new(0),
            open: Cell::new(true),
            generation: self.gui.ui.generation.get(),
        });
        *self.gui.ui.binding.borrow_mut() = Some(binding);
        Ok(())
    }

    /// Frees the native child and workers before the host may destroy its parent.
    fn destroy(&self) {
        self.gui.ui.destroy();
    }

    /// Validates the fallback DPI without applying an extra macOS backing-scale zoom.
    fn set_scale(&self, scale: f64) -> Result<(), PluginError> {
        if !scale.is_finite() || !(0.5..=8.0).contains(&scale) {
            return Err(PluginError::Message("Invalid CLAP GUI scale"));
        }
        self.gui.ui.scale.set(scale);
        if self.gui.ui.editor.borrow().is_none() && cfg!(windows) {
            let (width, height) = self.shared.instance.geometry.snapshot().native(scale);
            self.gui.ui.size.set(GuiSize {
                width: width as u32,
                height: height as u32,
            });
        }
        self.gui.ui.pending_scale.set(Some(scale));
        self.gui.ui.deferred.schedule()?;
        Ok(())
    }

    /// Reports saved geometry before attachment and accepted host geometry afterwards.
    fn get_size(&self) -> Option<GuiSize> {
        self.gui.ui.created.get().then(|| self.gui.ui.size.get())
    }

    /// Both logical dimensions can change independently.
    fn can_resize(&self) -> bool {
        true
    }

    /// No aspect-ratio restriction is needed by the shared editor layout.
    fn get_resize_hints(&self) -> Option<GuiResizeHints> {
        Some(GuiResizeHints {
            can_resize_horizontally: true,
            can_resize_vertically: true,
            strategy: AspectRatioStrategy::Disregard,
        })
    }

    /// Clamps oversized requests and rejects requests smaller than the usable minimum.
    fn adjust_size(&self, size: GuiSize) -> Option<GuiSize> {
        let scale = self.gui.ui.native_scale();
        let maximum = (LogicalSize {
            width: f64::from(MAX_EDITOR_SIZE.0),
            height: f64::from(MAX_EDITOR_SIZE.1),
        })
        .native(scale);
        let size = GuiSize {
            width: size.width.min(maximum.0 as u32),
            height: size.height.min(maximum.1 as u32),
        };
        self.gui.ui.valid_size(size).then_some(size)
    }

    /// Accepts complete bounded geometry and applies native changes outside active Iced callbacks.
    fn set_size(&self, size: GuiSize) -> Result<(), PluginError> {
        if !self.gui.ui.created.get() || !self.gui.ui.valid_size(size) {
            return Err(PluginError::Message("Invalid CLAP GUI dimensions"));
        }
        self.gui.ui.size.set(size);
        self.gui.ui.publish_geometry();
        self.gui.ui.pending_size.set(Some(size));
        self.gui.ui.deferred.schedule()?;
        Ok(())
    }

    /// Attaches the same Iced/wgpu editor to the host-owned native view on the main thread.
    fn set_parent(&self, window: Window) -> Result<(), PluginError> {
        let ui = &self.gui.ui;
        if !ui.created.get() || window.api_type() != native_api() || ui.editor.borrow().is_some() {
            return Err(PluginError::Message("Invalid CLAP GUI parent or lifecycle"));
        }
        let parent = window.as_generic_ptr();
        let binding = ui
            .binding
            .borrow()
            .clone()
            .ok_or(PluginError::Message("Missing CLAP GUI binding"))?;
        let size = ui.size.get();
        let generation = ui.generation.get();
        let _callback = UiCall::new(Rc::clone(ui));
        // SAFETY: CLAP guarantees this native parent stays alive until gui.destroy returns.
        let editor = unsafe {
            Editor::attach(
                parent,
                baseview::dpi::NativeSize::new(size.width, size.height).into(),
                ui.scale.get(),
                Arc::clone(&self.shared.instance.parameters),
                Arc::clone(&self.shared.instance.shared),
                binding,
            )
        }?;
        if !ui.created.get() || ui.generation.get() != generation {
            editor.close()?;
            return Err(PluginError::Message("CLAP GUI closed during native attachment"));
        }
        // The pinned AUv2 Cocoa wrapper parents an already-visible view but does not call CLAP gui.show.
        // Keep ordinary CLAP's hidden-until-show contract, and expose only that wrapper's attached child.
        if !is_auv2_wrapper(self.host) {
            editor.set_visible(false)?;
        }
        *ui.editor.borrow_mut() = Some(Rc::new(editor));
        Ok(())
    }

    /// Floating windows are deliberately not advertised.
    fn set_transient(&self, _window: Window) -> Result<(), PluginError> {
        Err(PluginError::Message("CLAP GUI is embedded only"))
    }

    /// Visibility changes retain the native window, analysis owner, and persistent editor state.
    fn show(&self) -> Result<(), PluginError> {
        if self.gui.ui.editor.borrow().is_none() {
            return Err(PluginError::Message("CLAP GUI has no parent"));
        }
        self.gui.ui.pending_visible.set(Some(true));
        self.gui.ui.deferred.schedule()?;
        Ok(())
    }

    /// Hiding never destroys the child or loses editor gesture/state ownership.
    fn hide(&self) -> Result<(), PluginError> {
        if self.gui.ui.editor.borrow().is_none() {
            return Err(PluginError::Message("CLAP GUI has no parent"));
        }
        self.gui.ui.pending_visible.set(Some(false));
        self.gui.ui.deferred.schedule()?;
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use clack_plugin::events::io::EventBuffer;

    /// In-process Clack host keeps transport tests independent of native windows and installed plugins.
    struct QueueHost;

    impl clack_host::host::HostHandlers for QueueHost {
        type Shared<'a> = ();
        type MainThread<'a> = ();
        type AudioProcessor<'a> = ();
    }

    /// Executes the production binding with Clack-owned host lifetime and an unattached GUI.
    fn with_binding(test: impl FnOnce(&Rc<Binding>, &Shared)) {
        let entry = clack_host::entry::PluginEntry::load_from_clack::<SinglePluginEntry<super::super::ImageControl>>(
            c"image-control-queue-test",
        )
        .expect("in-process entry");
        let host = clack_host::host::HostInfo::new("Queue test", "ImageControl", "https://example.invalid", "1")
            .expect("host metadata");
        let mut instance = clack_host::plugin::PluginInstance::<QueueHost>::new(
            |()| (),
            |()| (),
            &entry,
            c"com.esgrove.imagecontrol-rs-own",
            &host,
        )
        .expect("in-process plugin");
        let handle = instance.plugin_handle();
        // SAFETY: This pointer belongs to the live in-process ImageControl instance on its sole host thread.
        unsafe {
            clack_plugin::extensions::wrapper::PluginWrapper::<super::super::ImageControl>::handle(
                handle.as_raw_ptr(),
                |plugin| {
                    let main = plugin.main_thread();
                    main.create(GuiConfiguration {
                        api_type: native_api(),
                        is_floating: false,
                    })
                    .expect("unattached GUI lifetime");
                    let binding = main.gui.ui.binding.borrow().clone().expect("live binding");
                    test(&binding, plugin.shared());
                    main.destroy();
                    Ok(())
                },
            )
        }
        .expect("valid plugin wrapper");
    }

    /// Saturating automation cannot consume any of the 31 simultaneously open gestures' end slots.
    #[test]
    fn saturation_rejects_sound_publication_and_reserves_every_parameter_end() {
        with_binding(|binding, shared| {
            let parameters = &shared.instance.parameters;
            let accepted_limit = EDIT_CAPACITY - PARAMETER_COUNT;
            for id in 0..PARAMETER_COUNT as u32 {
                binding.begin_edit(id);
            }
            assert_eq!(binding.gestures.get().count_ones() as usize, PARAMETER_COUNT);
            for _ in PARAMETER_COUNT..accepted_limit {
                assert!(binding.publish_value(parameters, 0, 0.25));
            }
            assert_eq!(binding.edits.queue.len(), accepted_limit);
            let before = serde_json::to_vec(&parameters.snapshot()).expect("sound");
            for id in 0..PARAMETER_COUNT as u32 {
                assert!(!binding.publish_value(parameters, id, 0.75));
                binding.set_value(id, 0.75);
                binding.begin_edit(id);
            }
            assert_eq!(serde_json::to_vec(&parameters.snapshot()).expect("sound"), before);
            assert_eq!(binding.edits.queue.len(), accepted_limit);
            binding.close();
            assert_eq!(binding.gestures.get(), 0);
            assert_eq!(binding.edits.queue.len(), EDIT_CAPACITY);
            binding.close();
            for id in 0..PARAMETER_COUNT as u32 {
                binding.end_edit(id);
            }
            assert_eq!(binding.edits.queue.len(), EDIT_CAPACITY);
            let mut output = EventBuffer::new();
            binding
                .edits
                .flush(&mut OutputEvents::from_buffer(&mut output), parameters);
            let input = InputEvents::from_buffer(&output);
            assert_eq!(input.len() as usize, EDIT_CAPACITY);
            for id in 0..PARAMETER_COUNT as u32 {
                let begin = input
                    .get(id)
                    .expect("gesture begin")
                    .as_event::<ParamGestureBeginEvent>()
                    .expect("begin precedes values");
                assert_eq!(begin.param_id(), Some(ClapId::new(id)));
                let end = input
                    .get(accepted_limit as u32 + id)
                    .expect("reserved gesture end")
                    .as_event::<ParamGestureEndEvent>()
                    .expect("end follows values");
                assert_eq!(end.param_id(), Some(ClapId::new(id)));
            }
            for index in PARAMETER_COUNT..accepted_limit {
                let value = input
                    .get(index as u32)
                    .expect("admitted value")
                    .as_event::<ParamValueEvent>()
                    .expect("values stay between gesture edges");
                assert_eq!(value.param_id(), Some(ClapId::new(0)));
                assert_eq!(value.value(), -15.0);
            }
            assert!(binding.edits.queue.is_empty());
            assert!(binding.edits.retry.is_empty());
        });
    }

    /// The real editor cannot bypass saturated CLAP admission by publishing its own optimistic sound value.
    #[test]
    fn editor_updates_leave_sound_unchanged_when_clap_queue_is_saturated() {
        with_binding(|binding, shared| {
            let parameters = Arc::clone(&shared.instance.parameters);
            let mut editor = crate::editor::EditorState::for_host_test(
                Arc::clone(&parameters),
                Arc::clone(&shared.instance.shared),
                binding.clone(),
            );
            let accepted_limit = EDIT_CAPACITY - PARAMETER_COUNT;
            for _ in 0..accepted_limit - 1 {
                let _ = editor.update(crate::editor::Message::Change(0, 0.25));
            }
            assert_eq!(binding.edits.queue.len(), accepted_limit);
            assert_eq!(parameters.get(0), Some(0.25));
            let before = serde_json::to_vec(&parameters.snapshot()).expect("sound");
            let _ = editor.update(crate::editor::Message::Change(0, 0.75));
            assert_eq!(serde_json::to_vec(&parameters.snapshot()).expect("sound"), before);
            assert_eq!(binding.edits.queue.len(), accepted_limit);
            let _ = editor.update(crate::editor::Message::EndGesture);
            assert_eq!(binding.gestures.get(), 0);
            assert_eq!(binding.edits.queue.len(), accepted_limit + 1);
        });
    }

    /// A detached recall cancels queued value writes while retaining balanced host gesture edges.
    #[test]
    fn state_restore_discards_queued_values_without_losing_gesture_ends() {
        let parameters = Parameters::new();
        let edits = EditQueue::new();
        assert!(edits.queue.push(Edit::Begin(0)).is_ok());
        assert!(
            edits
                .queue
                .push(Edit::Value(0, 6.0, parameters.restore_generation()))
                .is_ok()
        );
        assert!(edits.queue.push(Edit::End(0)).is_ok());
        assert!(parameters.restore(crate::params::Snapshot::default()));
        let mut output = EventBuffer::new();
        edits.flush(&mut OutputEvents::from_buffer(&mut output), &parameters);
        let input = InputEvents::from_buffer(&output);
        assert_eq!(input.len(), 2);
        assert!(
            input
                .get(0)
                .expect("queued event")
                .as_event::<ParamGestureBeginEvent>()
                .is_some()
        );
        assert!(
            input
                .get(1)
                .expect("queued event")
                .as_event::<ParamGestureEndEvent>()
                .is_some()
        );
    }

    /// Host backpressure cannot reorder values ahead of gesture begin or lose a matching end.
    #[test]
    fn host_backpressure_preserves_order_and_balanced_gestures() {
        struct Reject;
        impl clack_plugin::events::io::OutputEventBuffer for Reject {
            fn try_push(&mut self, _event: &UnknownEvent) -> Result<(), clack_plugin::events::io::TryPushError> {
                Err(clack_plugin::events::io::TryPushError::new())
            }
        }
        let parameters = Parameters::new();
        let edits = EditQueue::new();
        assert!(edits.queue.push(Edit::Begin(0)).is_ok());
        assert!(
            edits
                .queue
                .push(Edit::Value(0, 6.0, parameters.restore_generation()))
                .is_ok()
        );
        assert!(edits.queue.push(Edit::End(0)).is_ok());
        edits.flush(&mut OutputEvents::from_buffer(&mut Reject), &parameters);
        assert_eq!(edits.retry.len(), 1);
        assert_eq!(edits.queue.len(), 2);
        let mut output = EventBuffer::new();
        edits.flush(&mut OutputEvents::from_buffer(&mut output), &parameters);
        let input = InputEvents::from_buffer(&output);
        assert_eq!(input.len(), 3);
        assert!(
            input
                .get(0)
                .expect("queued event")
                .as_event::<ParamGestureBeginEvent>()
                .is_some()
        );
        assert_eq!(
            input
                .get(1)
                .expect("queued event")
                .as_event::<ParamValueEvent>()
                .expect("queued event")
                .value(),
            6.0
        );
        assert!(
            input
                .get(2)
                .expect("queued event")
                .as_event::<ParamGestureEndEvent>()
                .is_some()
        );
    }
}
