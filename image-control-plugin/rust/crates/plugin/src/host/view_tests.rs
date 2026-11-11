//! Actual host bindings exercise COM reference-count reentry, recalled editor edits, and non-sound dirty state.

#![expect(
    unsafe_code,
    reason = "tests live bounded host COM interfaces and reference-count callbacks"
)]

use std::io::{Cursor, Read as _};

use vst3::Steinberg::Vst::{
    IComponentHandler2Trait, IComponentHandlerTrait, IComponentHandlerVtbl, IComponentTrait, IEditControllerTrait as _,
};
use vst3::Steinberg::{FUnknown, FUnknownVtbl, IBStream, IBStreamTrait, TUID, kNotImplemented};

use super::*;
use crate::editor::{EditorState, Message};
use crate::host::component::Component;
use crate::params::{GAIN_PARAMETER, Snapshot, band_parameter_id};

/// Builds production view ownership without a native child, GPU, or running application event loop.
fn unattached_view() -> PlugView {
    let imager = image_control_dsp::Imager::new();
    let logical = LogicalSize::default();
    PlugView {
        updates: UiOwner::new(ViewUpdates::new()),
        frame: UiOwner::new(Rc::new(RefCell::new(None))),
        interface: UiOwner::new(Rc::new(Cell::new(std::ptr::null_mut()))),
        owner: thread::current().id(),
        dimensions: Cell::new(logical.native(1.0)),
        logical: Cell::new(logical),
        geometry: Arc::new(EditorGeometry::default()),
        scale: Cell::new(1.0),
        host_sized: Cell::new(false),
        parameters: Arc::new(Parameters::new()),
        shared: imager.shared(),
        handler: Arc::new(Mutex::new(None)),
    }
}

#[test]
fn owner_thread_final_release_clears_interface_and_drops_ui_ownership() {
    let wrapper = ComWrapper::new(unattached_view());
    let updates = Rc::downgrade(&wrapper.updates);
    let frame = Rc::downgrade(&wrapper.frame);
    let interface = Rc::clone(&wrapper.interface);
    let view = wrapper.to_com_ptr::<IPlugView>().expect("view interface");
    wrapper.interface.set(view.as_ptr());
    drop(wrapper);
    drop(view);
    assert!(updates.upgrade().is_none());
    assert!(frame.upgrade().is_none());
    assert!(interface.get().is_null());
    assert_eq!(Rc::strong_count(&interface), 1);
}

#[test]
fn off_thread_final_release_leaks_every_ui_owner_without_touching_the_interface() {
    let wrapper = ComWrapper::new(unattached_view());
    let updates = Rc::downgrade(&wrapper.updates);
    let frame = Rc::downgrade(&wrapper.frame);
    let interface = Rc::downgrade(&wrapper.interface);
    let view = wrapper.to_com_ptr::<IPlugView>().expect("view interface");
    wrapper.interface.set(view.as_ptr());
    // Only the foreign ABI pointer crosses threads. No Send implementation exposes the UI-owned Rust resources.
    let address = view.into_raw().expose_provenance();
    drop(wrapper);
    thread::spawn(move || {
        let pointer = std::ptr::with_exposed_provenance_mut::<IPlugView>(address);
        // SAFETY: the test transfers its sole live COM reference into this deliberately invalid host release.
        // No owner-thread access occurs until this thread has joined, and the view must leak its UI resources.
        let view = unsafe { ComPtr::from_raw(pointer) }.expect("transferred view reference");
        drop(view);
    })
    .join()
    .expect("off-thread host release returns");
    assert_eq!(updates.strong_count(), 1);
    assert_eq!(frame.strong_count(), 1);
    assert_eq!(interface.strong_count(), 1);
    assert_eq!(
        interface
            .upgrade()
            .expect("leaked interface owner")
            .get()
            .expose_provenance(),
        address
    );
}

#[test]
#[cfg(windows)]
fn final_release_after_rejected_windows_removal_retains_all_ui_ownership() {
    let wrapper = ComWrapper::new(unattached_view());
    wrapper.updates.active.set(true);
    let updates = Rc::downgrade(&wrapper.updates);
    let frame = Rc::downgrade(&wrapper.frame);
    let interface = Rc::downgrade(&wrapper.interface);
    let call = wrapper.updates.host_call();
    let view = wrapper.to_com_ptr::<IPlugView>().expect("view interface");
    wrapper.interface.set(view.as_ptr());
    // SAFETY: this live owner-thread view must reject removal while its Iced-to-host callback remains active.
    assert_eq!(unsafe { view.removed() }, kResultFalse);
    drop(wrapper);
    drop(view);
    assert!(updates.upgrade().expect("retained update owner").active.get());
    assert!(interface.upgrade().expect("retained interface owner").get().is_null());
    drop(call);
    assert_eq!(updates.strong_count(), 1);
    assert_eq!(frame.strong_count(), 1);
    assert_eq!(interface.strong_count(), 1);
}

/// A real frame synchronously reenters view removal while the binding's host callback guard is active.
struct RemovalFrame(Rc<Cell<Option<tresult>>>);

impl Class for RemovalFrame {
    type Interfaces = (IPlugFrame,);
}

impl vst3::Steinberg::IPlugFrameTrait for RemovalFrame {
    unsafe fn resizeView(&self, view: *mut IPlugView, _: *mut ViewRect) -> tresult {
        // SAFETY: the binding passes the live owner-thread view retained by the test throughout this callback.
        let view = unsafe { ComRef::from_raw(view) }.expect("live reentrant view");
        // SAFETY: reentrant removal uses the same live interface and lets its platform admission rule decide.
        self.0.set(Some(unsafe { view.removed() }));
        kResultOk
    }
}

#[test]
fn reentrant_removal_keeps_ui_owners_live_until_the_host_callback_returns() {
    let wrapper = ComWrapper::new(unattached_view());
    let updates = Rc::downgrade(&wrapper.updates);
    let frame = Rc::downgrade(&wrapper.frame);
    let interface = Rc::downgrade(&wrapper.interface);
    let view = wrapper.to_com_ptr::<IPlugView>().expect("view interface");
    wrapper.interface.set(view.as_ptr());
    wrapper.updates.active.set(true);
    let removal = Rc::new(Cell::new(None));
    let host = ComWrapper::new(RemovalFrame(Rc::clone(&removal)));
    *wrapper.frame.borrow_mut() = Some(Rc::new(host.to_com_ptr::<IPlugFrame>().expect("frame interface")));
    drop(host);
    let binding = Binding {
        parameters: Arc::clone(&wrapper.parameters),
        handler: Arc::clone(&wrapper.handler),
        frame: Rc::clone(&wrapper.frame),
        interface: Rc::clone(&wrapper.interface),
        updates: Rc::downgrade(&wrapper.updates),
        gestures: Cell::new(0),
    };
    assert_eq!(binding.request_resize(1200, 900), cfg!(windows));
    assert_eq!(
        removal.get(),
        Some(if cfg!(windows) { kResultFalse } else { kResultOk })
    );
    assert_eq!(wrapper.updates.active.get(), cfg!(windows));
    assert!(updates.upgrade().is_some());
    assert_eq!(wrapper.interface.get(), view.as_ptr());
    // SAFETY: the host callback has returned, so both platforms accept removal on this live UI-owned view.
    assert_eq!(unsafe { view.removed() }, kResultOk);
    drop(binding);
    drop(wrapper);
    drop(view);
    assert!(updates.upgrade().is_none());
    assert!(frame.upgrade().is_none());
    assert!(interface.upgrade().is_none());
}

/// A bounded read-only host stream supplies independently authored state through the production component ABI.
struct StateStream(RefCell<Cursor<Vec<u8>>>);

impl Class for StateStream {
    type Interfaces = (IBStream,);
}

impl IBStreamTrait for StateStream {
    unsafe fn read(&self, buffer: *mut c_void, count: i32, read: *mut i32) -> tresult {
        let Ok(count) = usize::try_from(count) else {
            return kInvalidArgument;
        };
        if buffer.is_null() || count > 4096 || read.is_null() {
            return kInvalidArgument;
        }
        // SAFETY: the component supplies count writable bytes and a writable transfer count for this call.
        let bytes = unsafe { std::slice::from_raw_parts_mut(buffer.cast::<u8>(), count) };
        let count = self.0.borrow_mut().read(bytes).expect("in-memory stream read");
        // SAFETY: the validated transfer slot remains writable and count is bounded above.
        unsafe { read.write(i32::try_from(count).expect("bounded state transfer")) };
        kResultOk
    }
    unsafe fn write(&self, _: *mut c_void, _: i32, _: *mut i32) -> tresult {
        kNotImplemented
    }
    unsafe fn seek(&self, _: i64, _: i32, _: *mut i64) -> tresult {
        kNotImplemented
    }
    unsafe fn tell(&self, _: *mut i64) -> tresult {
        kNotImplemented
    }
}

/// Restores a valid independently authored state without closing the editor or calling a test-only parameter model.
fn restore(component: &Component) {
    let json = br#"{"version":5,"parameters":{"gain":0.25,"bands":[{"mid":0.5,"side":0.25},{},{},{}]}}"#;
    let mut bytes = u32::try_from(json.len()).expect("bounded state").to_le_bytes().to_vec();
    bytes.extend_from_slice(json);
    let stream = ComWrapper::new(StateStream(RefCell::new(Cursor::new(bytes))));
    let stream = stream.to_com_ptr::<IBStream>().expect("stream interface");
    // SAFETY: the component and readable stream remain live on this non-audio UI test thread.
    let result = unsafe { IComponentTrait::setState(component, stream.as_ptr()) };
    assert_eq!(result, kResultOk);
}

/// Selects one synchronous host callback for a single valid component recall.
#[derive(Clone, Copy, Eq, PartialEq)]
enum RestoreAt {
    Begin(u32),
    Perform(u32),
    End(u32),
}

/// Host observations remain accessible after every temporary COM owner has been released.
#[derive(Default)]
struct Evidence {
    events: RefCell<Vec<(u32, Option<f64>, bool)>>,
    dirty: Cell<usize>,
    dirty_threads: RefCell<Vec<ThreadId>>,
    restore_at: Cell<Option<RestoreAt>>,
}

/// A real COM host supports dirty notifications separately from sound gestures.
struct Handler {
    component: std::rc::Weak<Component>,
    evidence: Rc<Evidence>,
}

impl Handler {
    /// Consumes the scripted recall before entering production state code, allowing recursive host callbacks.
    fn restore_at(&self, phase: RestoreAt) {
        if self.evidence.restore_at.get() == Some(phase) {
            self.evidence.restore_at.set(None);
            restore(&self.component.upgrade().expect("live component"));
        }
    }
}

impl Class for Handler {
    type Interfaces = (IComponentHandler, IComponentHandler2);
}

impl IComponentHandlerTrait for Handler {
    unsafe fn beginEdit(&self, id: u32) -> tresult {
        self.evidence.events.borrow_mut().push((id, None, true));
        self.restore_at(RestoreAt::Begin(id));
        kResultOk
    }
    unsafe fn performEdit(&self, id: u32, value: f64) -> tresult {
        self.evidence.events.borrow_mut().push((id, Some(value), false));
        self.restore_at(RestoreAt::Perform(id));
        kResultOk
    }
    unsafe fn endEdit(&self, id: u32) -> tresult {
        self.evidence.events.borrow_mut().push((id, None, false));
        self.restore_at(RestoreAt::End(id));
        kResultOk
    }
    unsafe fn restartComponent(&self, _: i32) -> tresult {
        kResultOk
    }
}

impl IComponentHandler2Trait for Handler {
    unsafe fn setDirty(&self, state: TBool) -> tresult {
        assert_eq!(state, 1);
        self.evidence.dirty.set(self.evidence.dirty.get() + 1);
        self.evidence.dirty_threads.borrow_mut().push(thread::current().id());
        kResultOk
    }
    unsafe fn requestOpenEditor(&self, _: FIDString) -> tresult {
        kResultFalse
    }
    unsafe fn startGroupEdit(&self) -> tresult {
        kResultOk
    }
    unsafe fn finishGroupEdit(&self) -> tresult {
        kResultOk
    }
}

/// Owns the actual host binding and editor update state without requiring a native child or GPU.
fn editor(component: &Rc<Component>, evidence: &Rc<Evidence>) -> (EditorState, Rc<ViewUpdates>) {
    let host = ComWrapper::new(Handler {
        component: Rc::downgrade(component),
        evidence: Rc::clone(evidence),
    });
    let handler = host.to_com_ptr::<IComponentHandler>().expect("host handler");
    // SAFETY: handler installation retains this live host interface outside every plugin mutex.
    assert_eq!(unsafe { component.setComponentHandler(handler.as_ptr()) }, kResultOk);
    let updates = ViewUpdates::new();
    updates.active.set(true);
    let binding = Rc::new(Binding {
        parameters: Arc::clone(&component.parameters),
        handler: Arc::clone(&component.handler),
        frame: Rc::new(RefCell::new(None)),
        interface: Rc::new(Cell::new(std::ptr::null_mut())),
        updates: Rc::downgrade(&updates),
        gestures: Cell::new(0),
    });
    (
        EditorState::for_host_test(
            Arc::clone(&component.parameters),
            Arc::clone(&component.shared),
            binding,
        ),
        updates,
    )
}

/// Every opened participant must finish exactly once, even when recall occurs inside a host callback.
fn expect_balanced(evidence: &Evidence) {
    let events = evidence.events.borrow();
    for id in 0..crate::PARAMETER_COUNT as u32 {
        assert_eq!(
            events.iter().filter(|event| **event == (id, None, true)).count(),
            events.iter().filter(|event| **event == (id, None, false)).count(),
            "gesture {id}"
        );
    }
}

#[test]
fn recall_inside_real_begin_perform_and_end_callbacks_cannot_publish_stale_linked_values() {
    let mid = band_parameter_id(0, 0).expect("Mid");
    let side = band_parameter_id(0, 1).expect("Side");
    for phase in [
        RestoreAt::Begin(mid),
        RestoreAt::Begin(side),
        RestoreAt::Perform(mid),
        RestoreAt::End(side),
    ] {
        let component = Rc::new(Component::new());
        let evidence = Rc::new(Evidence::default());
        let (mut editor, _updates) = editor(&component, &evidence);
        let _ = editor.update(Message::Link(0, true));
        evidence.restore_at.set(Some(phase));
        let _ = editor.update(Message::NumericDraft(mid, "-6 dB".into()));
        let _ = editor.update(Message::NumericCommit(mid));
        assert_eq!(component.parameters.get(mid), Some(0.5));
        assert_eq!(component.parameters.get(side), Some(0.25));
        if phase == RestoreAt::Perform(mid) {
            assert!(
                !evidence
                    .events
                    .borrow()
                    .iter()
                    .any(|event| event.0 == side && event.1.is_some())
            );
        }
        // An acknowledged recall also ends the native numeric draft: a late Enter cannot replay its contents.
        let before = component.parameters.snapshot();
        let _ = editor.update(Message::NumericCommit(mid));
        assert_eq!(
            serde_json::to_vec(&component.parameters.snapshot()).expect("after"),
            serde_json::to_vec(&before).expect("before")
        );
        assert!(evidence.restore_at.get().is_none());
        expect_balanced(&evidence);
        assert_eq!(evidence.dirty.get(), 0);
    }
}

#[test]
fn recall_during_first_slider_change_cannot_be_revived_by_its_later_raw_press() {
    let component = Rc::new(Component::new());
    let evidence = Rc::new(Evidence::default());
    let (mut editor, _updates) = editor(&component, &evidence);
    evidence.restore_at.set(Some(RestoreAt::Begin(GAIN_PARAMETER)));
    let _ = editor.update(Message::Change(GAIN_PARAMETER, 0.8));
    // Native slider messages precede the listen_raw subscription for this same pointer press.
    let _ = editor.update(Message::Hold);
    let _ = editor.update(Message::Change(GAIN_PARAMETER, 0.9));
    assert_eq!(component.parameters.get(GAIN_PARAMETER), Some(0.25));
    let _ = editor.update(Message::EndGesture);
    let _ = editor.update(Message::Change(GAIN_PARAMETER, 0.75));
    let _ = editor.update(Message::EndGesture);
    assert_eq!(component.parameters.get(GAIN_PARAMETER), Some(0.75));
    expect_balanced(&evidence);
}

#[test]
fn recall_inside_toggle_begin_never_overwrites_the_recalled_switch() {
    let component = Rc::new(Component::new());
    let evidence = Rc::new(Evidence::default());
    let (mut editor, _updates) = editor(&component, &evidence);
    evidence
        .restore_at
        .set(Some(RestoreAt::Begin(crate::params::MONO_PARAMETER)));
    let _ = editor.update(Message::Toggle(crate::params::MONO_PARAMETER));
    assert_eq!(component.parameters.get(crate::params::MONO_PARAMETER), Some(0.0));
    assert!(!evidence.events.borrow().iter().any(|event| event.1.is_some()));
    expect_balanced(&evidence);
}

#[test]
fn recall_cancels_a_dirty_draft_and_remaining_held_drag_before_the_next_refresh() {
    let component = Rc::new(Component::new());
    let evidence = Rc::new(Evidence::default());
    let (mut editor, _updates) = editor(&component, &evidence);
    let _ = editor.update(Message::NumericDraft(GAIN_PARAMETER, "6 dB".into()));
    restore(&component);
    let _ = editor.update(Message::NumericCommit(GAIN_PARAMETER));
    assert_eq!(component.parameters.get(GAIN_PARAMETER), Some(0.25));
    assert!(evidence.events.borrow().is_empty());
    let _ = editor.update(Message::Hold);
    let _ = editor.update(Message::Change(GAIN_PARAMETER, 0.8));
    restore(&component);
    let _ = editor.update(Message::Refresh);
    let _ = editor.update(Message::Change(GAIN_PARAMETER, 0.9));
    assert_eq!(component.parameters.get(GAIN_PARAMETER), Some(0.25));
    let _ = editor.update(Message::EndGesture);
    let _ = editor.update(Message::Hold);
    let _ = editor.update(Message::Change(GAIN_PARAMETER, 0.75));
    let _ = editor.update(Message::EndGesture);
    assert_eq!(component.parameters.get(GAIN_PARAMETER), Some(0.75));
    expect_balanced(&evidence);
}

#[test]
fn changed_persisted_preferences_dirty_the_host_without_sound_gestures_or_restore_notifications() {
    let component = Rc::new(Component::new());
    let evidence = Rc::new(Evidence::default());
    let (mut editor, _updates) = editor(&component, &evidence);
    for message in [
        Message::Tap(image_control_dsp::AnalysisTap::Pre),
        Message::Persistence(1.25),
        Message::Contrast(0.25),
        Message::Colour(image_control_dsp::ColourMode::Phase),
        Message::ResetAppearance,
    ] {
        let before = evidence.dirty.get();
        let _ = editor.update(message.clone());
        assert_eq!(evidence.dirty.get(), before + 1);
        let _ = editor.update(message);
        assert_eq!(evidence.dirty.get(), before + 1);
    }
    let before = evidence.dirty.get();
    let _ = editor.update(Message::Contrast(f32::NAN));
    let _ = editor.update(Message::Correlation);
    assert_eq!(evidence.dirty.get(), before + 1);
    let before = evidence.dirty.get();
    std::thread::scope(|scope| {
        let component = &*component;
        scope
            .spawn(move || restore(component))
            .join()
            .expect("detached state restore");
    });
    let _ = editor.update(Message::Refresh);
    assert_eq!(evidence.dirty.get(), before);
    assert!(evidence.events.borrow().is_empty());
    assert!(
        evidence
            .dirty_threads
            .borrow()
            .iter()
            .all(|owner| *owner == thread::current().id())
    );
}

#[test]
fn accepted_changed_geometry_dirties_once_and_invalid_or_echoed_geometry_does_not() {
    let component = Rc::new(Component::new());
    let evidence = Rc::new(Evidence::default());
    let (_editor, _updates) = editor(&component, &evidence);
    // SAFETY: creation transfers an owned view reference on its UI owner thread.
    let view = unsafe { ComPtr::from_raw(component.createView(c"editor".as_ptr())) }.expect("view");
    let mut size = ViewRect {
        left: 0,
        top: 0,
        right: 1200,
        bottom: 900,
    };
    // SAFETY: the owner-thread view and writable rectangle remain live throughout these calls.
    unsafe {
        assert_eq!(view.onSize(&raw mut size), kResultOk);
        assert_eq!(evidence.dirty.get(), 1);
        assert_eq!(view.onSize(&raw mut size), kResultOk);
        size.right = 1;
        assert_eq!(view.onSize(&raw mut size), kResultFalse);
    }
    assert_eq!(evidence.dirty.get(), 1);
}

/// A live raw ABI fixture owns its storage in the test, so callbacks can observe locks without blocking or panicking.
#[repr(C)]
struct ReferenceHandler {
    interface: IComponentHandler,
    component: Rc<Component>,
    references: Cell<u32>,
    added: Cell<usize>,
    blocked: Cell<bool>,
}

impl ReferenceHandler {
    /// Tests reentry only after a nonblocking lock probe, making even the regressed implementation timeout-proof.
    fn reenter(&self) {
        if self.component.handler.try_lock().is_err() {
            self.blocked.set(true);
            return;
        }
        // SAFETY: this live component accepts a null handler and the lock probe above retained no guard.
        unsafe { self.component.setComponentHandler(std::ptr::null_mut()) };
    }
}

/// Borrows the fixture whose ABI interface is its first repr(C) field, valid for the entire test.
unsafe fn reference_handler<'a>(this: *mut FUnknown) -> &'a ReferenceHandler {
    // SAFETY: only the statically installed fixture vtable calls this with its first interface address.
    unsafe { &*this.cast::<ReferenceHandler>() }
}

unsafe extern "system" fn reference_query(_: *mut FUnknown, _: *const TUID, output: *mut *mut c_void) -> tresult {
    if !output.is_null() {
        // SAFETY: queryInterface provides a writable output slot.
        unsafe { output.write(std::ptr::null_mut()) };
    }
    kResultFalse
}
unsafe extern "system" fn reference_add(this: *mut FUnknown) -> u32 {
    // SAFETY: this interface belongs to the live raw fixture.
    let host = unsafe { reference_handler(this) };
    host.references.set(host.references.get() + 1);
    host.added.set(host.added.get() + 1);
    host.reenter();
    host.references.get()
}
unsafe extern "system" fn reference_release(this: *mut FUnknown) -> u32 {
    // SAFETY: this interface belongs to the live raw fixture.
    let host = unsafe { reference_handler(this) };
    host.references.set(host.references.get() - 1);
    host.reenter();
    host.references.get()
}
unsafe extern "system" fn reference_edit(_: *mut IComponentHandler, _: u32) -> tresult {
    kResultOk
}
unsafe extern "system" fn reference_perform(_: *mut IComponentHandler, _: u32, _: f64) -> tresult {
    kResultOk
}
unsafe extern "system" fn reference_restart(_: *mut IComponentHandler, _: i32) -> tresult {
    kResultOk
}

const REFERENCE_VTABLE: IComponentHandlerVtbl = IComponentHandlerVtbl {
    base: FUnknownVtbl {
        queryInterface: reference_query,
        addRef: reference_add,
        release: reference_release,
    },
    beginEdit: reference_edit,
    performEdit: reference_perform,
    endEdit: reference_edit,
    restartComponent: reference_restart,
};

#[test]
fn handler_addref_and_release_reentry_never_execute_under_the_handler_mutex() {
    let component = Rc::new(Component::new());
    let mut host = ReferenceHandler {
        interface: IComponentHandler {
            vtbl: &REFERENCE_VTABLE,
        },
        component: Rc::clone(&component),
        references: Cell::new(1),
        added: Cell::new(0),
        blocked: Cell::new(false),
    };
    // SAFETY: storage remains live until every plugin-held reference below is explicitly released.
    let result = unsafe { component.setComponentHandler(&raw mut host.interface) };
    assert_eq!(result, kResultOk);
    let updates = ViewUpdates::new();
    updates.active.set(true);
    let binding = Binding {
        parameters: Arc::clone(&component.parameters),
        handler: Arc::clone(&component.handler),
        frame: Rc::new(RefCell::new(None)),
        interface: Rc::new(Cell::new(std::ptr::null_mut())),
        updates: Rc::downgrade(&updates),
        gestures: Cell::new(0),
    };
    let added = host.added.get();
    binding.begin_edit(GAIN_PARAMETER);
    binding.set_value(GAIN_PARAMETER, 0.25);
    binding.end_edit(GAIN_PARAMETER);
    binding.mark_dirty();
    let retained = binding.handler().expect("retained host");
    assert_eq!(
        host.added.get(),
        added,
        "sharing an existing COM owner calls no host AddRef"
    );
    // SAFETY: clearing the handler leaves retained as its last plugin-owned reference, released on this same UI thread.
    unsafe { component.setComponentHandler(std::ptr::null_mut()) };
    drop(retained);
    assert!(
        !host.blocked.get(),
        "host reference-count code entered while the mutex was locked"
    );
    assert_eq!(host.references.get(), 1);
}

#[test]
fn unsuccessful_parameter_restore_does_not_advance_editor_recall_generation() {
    let parameters = Parameters::new();
    assert!(parameters.set(GAIN_PARAMETER, 0.5));
    assert_eq!(parameters.restore_generation(), 0);
    let invalid = Snapshot {
        gain: f64::NAN,
        ..Snapshot::default()
    };
    assert!(!parameters.restore(invalid));
    assert_eq!(parameters.restore_generation(), 0);
    assert!(parameters.restore(Snapshot::default()));
    assert_eq!(parameters.restore_generation(), 1);
}
