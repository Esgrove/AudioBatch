//! UI-thread-only VST3 view attachment, geometry, scale support, and reentrancy-safe host gestures.
//!
//! Native editor and frame references are never touched by audio processing.
//! A broken host's final off-thread release leaks native resources rather than destroying them on the wrong thread.

#![expect(unsafe_code, reason = "implements VST3 native view attachment and host UI callbacks")]

use std::cell::{Cell, RefCell};
use std::ffi::{CStr, c_void};
use std::mem::ManuallyDrop;
use std::rc::Rc;
use std::sync::{Arc, Mutex};
use std::thread::{self, ThreadId};

use image_control_dsp::SharedState;
use vst3::Steinberg::Vst::{IComponentHandler, IComponentHandlerTrait as _};
use vst3::Steinberg::{
    FIDString, IPlugFrame, IPlugFrameTrait as _, IPlugView, IPlugViewContentScaleSupport,
    IPlugViewContentScaleSupportTrait, IPlugViewTrait, TBool, ViewRect, kInvalidArgument, kResultFalse, kResultOk,
    kResultTrue, tresult,
};
use vst3::{Class, ComPtr, ComRef, ComWrapper};

use crate::iced_bridge::Editor;
use crate::params::{EditorBinding, Parameters, parameter_spec};

#[path = "view_deferred.rs"]
mod deferred;
use deferred::ViewUpdates;

/// Default logical dimensions before host resize.
const EDITOR_SIZE: (i32, i32) = (crate::editor::EDITOR_SIZE.0 as i32, crate::editor::EDITOR_SIZE.1 as i32);
/// Minimum editor dimensions that keep the four bands usable.
const MINIMUM_SIZE: (i32, i32) = (
    crate::editor::MIN_EDITOR_SIZE.0 as i32,
    crate::editor::MIN_EDITOR_SIZE.1 as i32,
);
/// Matches the C++ editor's maximum logical dimensions before platform-native DPI conversion.
const MAXIMUM_SIZE: (i32, i32) = (4096, 4096);

/// Native VST3 rectangles are macOS points or Windows physical pixels, not renderer backing pixels.
/// Round both bounds identically to the editor's logical-to-native resize requests.
fn native_bounds(scale: f64) -> ((i32, i32), (i32, i32)) {
    let dimension = |value: i32| (f64::from(value) * scale).round() as i32;
    (
        (dimension(MINIMUM_SIZE.0), dimension(MINIMUM_SIZE.1)),
        (dimension(MAXIMUM_SIZE.0), dimension(MAXIMUM_SIZE.1)),
    )
}

/// Validates sizes from hosts and native child requests with the same current-DPI contract.
fn valid_dimensions(width: i32, height: i32, scale: f64) -> bool {
    let (minimum, maximum) = native_bounds(scale);
    (minimum.0..=maximum.0).contains(&width) && (minimum.1..=maximum.1).contains(&height)
}

/// COM view whose native resources belong to the constructing UI thread.
pub(super) struct PlugView {
    /// Editor destruction is restricted to the creating UI thread.
    updates: ManuallyDrop<Rc<ViewUpdates>>,
    /// Host frame lifetime is retained until replacement or view teardown.
    frame: ManuallyDrop<Rc<RefCell<Option<ComPtr<IPlugFrame>>>>>,
    /// Borrowed self interface, valid while the view exists. Rc destruction stays on the UI owner.
    interface: ManuallyDrop<Rc<Cell<*mut IPlugView>>>,
    /// Thread ownership check, never called from audio.
    owner: ThreadId,
    /// Logical/host dimensions, interpreted by the native editor.
    dimensions: Cell<(i32, i32)>,
    /// Most recently supplied host scale, including before attachment.
    scale: Cell<f64>,
    /// Explicit pre-attachment host geometry takes precedence over scaling the default editor size.
    host_sized: Cell<bool>,
    /// Authoritative parameters, shared with the same controller and processor.
    parameters: Arc<Parameters>,
    /// Audio analysis transport, independent of mutable DSP history.
    shared: Arc<SharedState>,
    /// UI-only host callback ownership.
    handler: Arc<Mutex<Option<ComPtr<IComponentHandler>>>>,
}

/// UI gesture routing without retaining the editor or holding locks across host calls.
struct Binding {
    /// Parameters are read by the editor directly from this same authoritative allocation.
    parameters: Arc<Parameters>,
    /// Host handler, cloned under a short lock before callback invocation.
    handler: Arc<Mutex<Option<ComPtr<IComponentHandler>>>>,
    /// Host frame shared only on the UI thread and cloned before any callback.
    frame: Rc<RefCell<Option<ComPtr<IPlugFrame>>>>,
    /// Borrowed live view interface for resizeView, without a self-reference cycle.
    interface: Rc<Cell<*mut IPlugView>>,
    /// Weak owner prevents a program/window cycle while tracking host callback depth.
    updates: std::rc::Weak<ViewUpdates>,
    /// Fixed parameter set bounds close-time gesture balancing without borrowing Iced state.
    gestures: Cell<u32>,
}

impl PlugView {
    /// Creates an owned COM view and records a non-owning self pointer for host resize requests.
    pub(super) fn create(
        parameters: Arc<Parameters>,
        shared: Arc<SharedState>,
        handler: Arc<Mutex<Option<ComPtr<IComponentHandler>>>>,
    ) -> *mut IPlugView {
        let wrapper = ComWrapper::new(Self {
            updates: ManuallyDrop::new(ViewUpdates::new()),
            frame: ManuallyDrop::new(Rc::new(RefCell::new(None))),
            interface: ManuallyDrop::new(Rc::new(Cell::new(std::ptr::null_mut()))),
            owner: thread::current().id(),
            dimensions: Cell::new(EDITOR_SIZE),
            scale: Cell::new(1.0),
            host_sized: Cell::new(false),
            parameters,
            shared,
            handler,
        });
        let Some(view) = wrapper.to_com_ptr::<IPlugView>() else {
            return std::ptr::null_mut();
        };
        wrapper.interface.set(view.as_ptr());
        view.into_raw()
    }

    /// Checks ownership before touching any native or RefCell-backed state.
    fn is_owner(&self) -> bool {
        self.owner == thread::current().id()
    }

    /// Requests current geometry after releasing all view borrows, allowing synchronous host onSize callbacks.
    fn request_size(&self) {
        let frame = self.frame.borrow().clone();
        if let Some(frame) = frame {
            let (width, height) = self.dimensions.get();
            let mut size = ViewRect {
                left: 0,
                top: 0,
                right: width,
                bottom: height,
            };
            // SAFETY: frame owns a live host reference, self pointer stays valid for this callback,
            // and the rectangle remains writable. No RefCell or handler lock is held across reentry.
            unsafe { frame.resizeView(self.interface.get(), &raw mut size) };
        }
    }
}

impl Class for PlugView {
    type Interfaces = (IPlugView, IPlugViewContentScaleSupport);
}

impl Drop for PlugView {
    fn drop(&mut self) {
        if self.is_owner() {
            // SAFETY: native editor and frame destruction occur only on their constructing UI thread.
            unsafe {
                let removed = self.updates.remove();
                self.interface.set(std::ptr::null_mut());
                // A final release after rejected Win32 removal is invalid.
                // Destroying a focused child inside Iced's active poll is unsafe.
                // Retain native ownership rather than dispatch focus callbacks through a borrowed runtime.
                // This is not a clean teardown.
                if removed.is_err() {
                    return;
                }
                ManuallyDrop::drop(&mut self.updates);
                ManuallyDrop::drop(&mut self.frame);
                ManuallyDrop::drop(&mut self.interface);
            }
        }
    }
}

impl IPlugViewContentScaleSupportTrait for PlugView {
    unsafe fn setContentScaleFactor(&self, factor: f32) -> tresult {
        if !self.is_owner() || !factor.is_finite() || !(0.5..=8.0).contains(&factor) {
            return kInvalidArgument;
        }
        if self.updates.scale(f64::from(factor)).is_err() {
            return kResultFalse;
        }
        self.scale.set(f64::from(factor));
        if cfg!(windows) && !self.updates.active.get() && !self.host_sized.get() {
            self.dimensions.set((
                (f64::from(EDITOR_SIZE.0) * f64::from(factor)).round() as i32,
                (f64::from(EDITOR_SIZE.1) * f64::from(factor)).round() as i32,
            ));
        }
        kResultOk
    }
}

impl IPlugViewTrait for PlugView {
    unsafe fn isPlatformTypeSupported(&self, platform: FIDString) -> tresult {
        if platform.is_null() {
            return kInvalidArgument;
        }
        // SAFETY: VST3 supplies a terminated platform name valid for this call.
        let platform = unsafe { CStr::from_ptr(platform) };
        if (cfg!(target_os = "macos") && platform == c"NSView") || (cfg!(windows) && platform == c"HWND") {
            kResultTrue
        } else {
            kResultFalse
        }
    }

    unsafe fn attached(&self, parent: *mut c_void, platform: FIDString) -> tresult {
        if !self.is_owner() || parent.is_null() {
            return kInvalidArgument;
        }
        // SAFETY: forwards the same host-owned platform name to validation.
        if unsafe { self.isPlatformTypeSupported(platform) } != kResultTrue {
            return kResultFalse;
        }
        let Ok(mut slot) = self.updates.editor.try_borrow_mut() else {
            return kResultFalse;
        };
        if slot.is_some() {
            return kResultFalse;
        }
        let (width, height) = self.dimensions.get();
        let binding: Rc<dyn EditorBinding> = Rc::new(Binding {
            parameters: Arc::clone(&self.parameters),
            handler: Arc::clone(&self.handler),
            frame: Rc::clone(&self.frame),
            interface: Rc::clone(&self.interface),
            updates: Rc::downgrade(&self.updates),
            gestures: Cell::new(0),
        });
        let size = if self.host_sized.get() {
            baseview::dpi::NativeSize::new(width as u32, height as u32).into()
        } else {
            // A default editor opens at its intended logical size even when the host has not supplied DPI yet.
            baseview::dpi::LogicalSize::new(f64::from(EDITOR_SIZE.0), f64::from(EDITOR_SIZE.1)).into()
        };
        // SAFETY: VST3 guarantees a live parent until removed, and this editor's methods and drop stay on its UI owner.
        let Ok(editor) = (unsafe {
            Editor::attach(
                parent,
                size,
                self.scale.get(),
                Arc::clone(&self.parameters),
                Arc::clone(&self.shared),
                binding,
            )
        }) else {
            return kResultFalse;
        };
        if editor.set_scale_factor(self.scale.get()).is_err() {
            return kResultFalse;
        }
        if let Ok(native) = editor.native_size()
            && let (Ok(width), Ok(height)) = (i32::try_from(native.width), i32::try_from(native.height))
        {
            self.dimensions.set((width, height));
        }
        *slot = Some(Rc::new(editor));
        self.updates.active.set(true);
        drop(slot);
        self.request_size();
        kResultOk
    }

    unsafe fn removed(&self) -> tresult {
        if !self.is_owner() {
            return kInvalidArgument;
        }
        if self.updates.remove().is_err() {
            kResultFalse
        } else {
            kResultOk
        }
    }

    unsafe fn onWheel(&self, _distance: f32) -> tresult {
        kResultFalse
    }
    unsafe fn onKeyDown(&self, _key: u16, _code: i16, _modifiers: i16) -> tresult {
        kResultFalse
    }
    unsafe fn onKeyUp(&self, _key: u16, _code: i16, _modifiers: i16) -> tresult {
        kResultFalse
    }

    unsafe fn getSize(&self, size: *mut ViewRect) -> tresult {
        if size.is_null() || !self.is_owner() {
            return kInvalidArgument;
        }
        let (width, height) = self.dimensions.get();
        // SAFETY: the host provides one writable ViewRect.
        unsafe {
            size.write(ViewRect {
                left: 0,
                top: 0,
                right: width,
                bottom: height,
            });
        };
        kResultOk
    }

    unsafe fn onSize(&self, size: *mut ViewRect) -> tresult {
        if size.is_null() || !self.is_owner() {
            return kInvalidArgument;
        }
        // SAFETY: the host provides one readable ViewRect.
        let size = unsafe { &*size };
        let (Some(width), Some(height)) = (size.right.checked_sub(size.left), size.bottom.checked_sub(size.top)) else {
            return kInvalidArgument;
        };
        if !valid_dimensions(width, height, self.updates.native_scale(self.scale.get())) {
            return kResultFalse;
        }
        if self.updates.resize(width as u32, height as u32).is_err() {
            return kResultFalse;
        }
        self.dimensions.set((width, height));
        self.host_sized.set(true);
        kResultOk
    }

    unsafe fn onFocus(&self, _state: TBool) -> tresult {
        kResultOk
    }

    unsafe fn setFrame(&self, frame: *mut IPlugFrame) -> tresult {
        if !self.is_owner() {
            return kInvalidArgument;
        }
        // SAFETY: the host provides a live frame or null. The retained COM reference owns its lifetime.
        let frame = unsafe { ComRef::from_raw(frame) }.map(|frame| frame.to_com_ptr());
        let Ok(mut slot) = self.frame.try_borrow_mut() else {
            return kResultFalse;
        };
        let old = std::mem::replace(&mut *slot, frame);
        drop(slot);
        drop(old);
        kResultOk
    }

    unsafe fn canResize(&self) -> tresult {
        kResultTrue
    }

    unsafe fn checkSizeConstraint(&self, rect: *mut ViewRect) -> tresult {
        if rect.is_null() || !self.is_owner() {
            return kInvalidArgument;
        }
        // SAFETY: the host provides one writable ViewRect.
        let rect = unsafe { &mut *rect };
        let (minimum, maximum) = native_bounds(self.updates.native_scale(self.scale.get()));
        let width = rect.right.saturating_sub(rect.left).clamp(minimum.0, maximum.0);
        let height = rect.bottom.saturating_sub(rect.top).clamp(minimum.1, maximum.1);
        let (Some(right), Some(bottom)) = (rect.left.checked_add(width), rect.top.checked_add(height)) else {
            return kInvalidArgument;
        };
        rect.right = right;
        rect.bottom = bottom;
        kResultOk
    }
}

impl Binding {
    /// Clones a host reference before calling arbitrary, potentially reentrant host code.
    fn handler(&self) -> Option<ComPtr<IComponentHandler>> {
        self.handler
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
            .clone()
    }
}

impl EditorBinding for Binding {
    fn begin_edit(&self, id: u32) {
        let Some(updates) = self.updates.upgrade().filter(|updates| updates.active.get()) else {
            return;
        };
        let _host_call = updates.host_call();
        if parameter_spec(id).is_some()
            && let Some(handler) = self.handler()
        {
            let bit = 1_u32 << id;
            if self.gestures.get() & bit != 0 {
                return;
            }
            self.gestures.set(self.gestures.get() | bit);
            // SAFETY: only the UI invokes this binding, and the handler remains owned throughout the call.
            unsafe { handler.beginEdit(id) };
        }
    }

    fn set_value(&self, id: u32, normalized: f64) {
        let Some(updates) = self.updates.upgrade().filter(|updates| updates.active.get()) else {
            return;
        };
        let _host_call = updates.host_call();
        if self.parameters.set(id, normalized)
            && let Some(handler) = self.handler()
        {
            let normalized = self.parameters.get(id).unwrap_or_default();
            // SAFETY: the handler remains owned and no mutex or view borrow is held across host reentry.
            unsafe { handler.performEdit(id, normalized) };
        }
    }

    fn end_edit(&self, id: u32) {
        let Some(updates) = self.updates.upgrade() else {
            return;
        };
        let _host_call = updates.host_call();
        if parameter_spec(id).is_some()
            && let Some(handler) = self.handler()
        {
            let bit = 1_u32 << id;
            if self.gestures.get() & bit == 0 {
                return;
            }
            self.gestures.set(self.gestures.get() & !bit);
            // SAFETY: UI gesture ownership balances begin/end, including interrupted teardown.
            unsafe { handler.endEdit(id) };
        }
    }

    fn close(&self) {
        // Clear ownership before arbitrary endEdit reentry, including recursive removed calls.
        let gestures = self.gestures.replace(0);
        if let Some(handler) = self.handler() {
            for id in 0..crate::PARAMETER_COUNT as u32 {
                if gestures & (1_u32 << id) != 0 {
                    // SAFETY: this is the UI owner's retained handler and each bit owns exactly one open gesture.
                    unsafe { handler.endEdit(id) };
                }
            }
        }
    }

    fn request_resize(&self, width: u32, height: u32) -> bool {
        let Some(updates) = self.updates.upgrade().filter(|updates| updates.active.get()) else {
            return false;
        };
        let _host_call = updates.host_call();
        let (Ok(width), Ok(height)) = (i32::try_from(width), i32::try_from(height)) else {
            return false;
        };
        if !valid_dimensions(width, height, updates.native_scale(1.0)) {
            return false;
        }
        let frame = self.frame.borrow().clone();
        let Some(frame) = frame else {
            return false;
        };
        let view = self.interface.get();
        if view.is_null() {
            return false;
        }
        let mut size = ViewRect {
            left: 0,
            top: 0,
            right: width,
            bottom: height,
        };
        // SAFETY: this UI callback retains the frame and live view.
        // No RefCell borrow or handler mutex spans resizeView,
        // which may synchronously reenter onSize or save state.
        unsafe { frame.resizeView(view, &raw mut size) == kResultOk && updates.active.get() }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Builds a real COM view without attaching a native child or requiring an application event loop.
    fn view() -> ComPtr<IPlugView> {
        let imager = image_control_dsp::Imager::new();
        let pointer = PlugView::create(Arc::new(Parameters::new()), imager.shared(), Arc::new(Mutex::new(None)));
        // SAFETY: create transfers one owning reference for its declared IPlugView interface.
        unsafe { ComPtr::from_raw(pointer) }.expect("view interface")
    }

    #[test]
    fn host_constraints_and_on_size_share_bounds_and_preserve_valid_origins() {
        let view = view();
        let mut size = ViewRect {
            left: 15,
            top: 27,
            right: 16,
            bottom: 28,
        };
        let (minimum, maximum) = native_bounds(1.0);
        // SAFETY: Every COM call uses a live owned view and one writable rectangle on its creator thread.
        unsafe {
            assert_eq!(view.canResize(), kResultTrue);
            assert_eq!(view.checkSizeConstraint(&raw mut size), kResultOk);
            assert_eq!((size.left, size.top), (15, 27));
            assert_eq!((size.right, size.bottom), (15 + minimum.0, 27 + minimum.1));
            assert_eq!(view.onSize(&raw mut size), kResultOk);
            assert_eq!(view.getSize(&raw mut size), kResultOk);
            assert_eq!(
                (size.left, size.top, size.right, size.bottom),
                (0, 0, minimum.0, minimum.1)
            );
            size.right = maximum.0 + 1;
            assert_eq!(view.onSize(&raw mut size), kResultFalse);
            size.left = i32::MIN;
            size.right = i32::MAX;
            assert_eq!(view.onSize(&raw mut size), kInvalidArgument);
            size.left = i32::MAX;
            assert_eq!(view.checkSizeConstraint(&raw mut size), kInvalidArgument);
            assert_eq!(view.checkSizeConstraint(std::ptr::null_mut()), kInvalidArgument);
        }
    }

    #[test]
    fn pre_attachment_scale_preserves_macos_points_and_scales_windows_defaults() {
        let view = view();
        let scale = view
            .cast::<IPlugViewContentScaleSupport>()
            .expect("content scale interface");
        let mut size = ViewRect {
            left: 0,
            top: 0,
            right: 0,
            bottom: 0,
        };
        // SAFETY: The real COM interfaces and writable rectangle remain live on their owner thread.
        unsafe {
            assert_eq!(scale.setContentScaleFactor(2.0), kResultOk);
            assert_eq!(view.getSize(&raw mut size), kResultOk);
            let factor = if cfg!(windows) { 2 } else { 1 };
            assert_eq!(
                (size.right, size.bottom),
                (EDITOR_SIZE.0 * factor, EDITOR_SIZE.1 * factor)
            );
            assert_eq!(scale.setContentScaleFactor(f32::NAN), kInvalidArgument);
            assert_eq!(scale.setContentScaleFactor(0.0), kInvalidArgument);
            let mut supplied = ViewRect {
                left: 0,
                top: 0,
                right: 2000,
                bottom: 1600,
            };
            assert_eq!(view.onSize(&raw mut supplied), kResultOk);
            assert_eq!(scale.setContentScaleFactor(1.25), kResultOk);
            assert_eq!(view.getSize(&raw mut size), kResultOk);
            assert_eq!((size.right, size.bottom), (2000, 1600));
        }
    }

    #[test]
    fn logical_constraints_scale_for_native_windows_pixels_without_changing_point_contract() {
        for scale in [1.0, 1.25, 1.5, 2.0, 8.0] {
            let (minimum, maximum) = native_bounds(scale);
            assert_eq!(minimum.0, (f64::from(MINIMUM_SIZE.0) * scale).round() as i32);
            assert_eq!(maximum.0, (4096.0 * scale).round() as i32);
            assert_eq!(maximum.1, (4096.0 * scale).round() as i32);
            assert!(valid_dimensions(minimum.0, minimum.1, scale));
            assert!(!valid_dimensions(minimum.0 - 1, minimum.1, scale));
            assert!(!valid_dimensions(maximum.0 + 1, maximum.1, scale));
        }
    }

    /// A rejecting frame may synchronously query the view without changing its accepted native geometry.
    struct RejectFrame(Cell<*mut IPlugView>);

    impl Class for RejectFrame {
        type Interfaces = (IPlugFrame,);
    }

    impl vst3::Steinberg::IPlugFrameTrait for RejectFrame {
        unsafe fn resizeView(&self, view: *mut IPlugView, _size: *mut ViewRect) -> tresult {
            assert_eq!(view, self.0.get());
            // SAFETY: The caller retains this same live view through the synchronous frame callback.
            let view = unsafe { ComRef::from_raw(view) }.expect("live callback view");
            let mut size = ViewRect {
                left: 0,
                top: 0,
                right: 0,
                bottom: 0,
            };
            // SAFETY: The owner-thread view and output rectangle are valid across this query.
            assert_eq!(unsafe { view.getSize(&raw mut size) }, kResultOk);
            assert_eq!((size.right, size.bottom), EDITOR_SIZE);
            kResultFalse
        }
    }

    #[test]
    fn host_rejection_and_missing_frame_never_queue_optimistic_native_geometry() {
        let view = view();
        let updates = ViewUpdates::new();
        updates.active.set(true);
        let binding = Binding {
            parameters: Arc::new(Parameters::new()),
            handler: Arc::new(Mutex::new(None)),
            frame: Rc::new(RefCell::new(None)),
            interface: Rc::new(Cell::new(view.as_ptr())),
            updates: Rc::downgrade(&updates),
            gestures: Cell::new(0),
        };
        assert!(!binding.request_resize(1200, 900));
        let frame = ComWrapper::new(RejectFrame(Cell::new(view.as_ptr())))
            .to_com_ptr::<IPlugFrame>()
            .expect("frame interface");
        *binding.frame.borrow_mut() = Some(frame);
        assert!(!binding.request_resize(1200, 900));
        assert!(!binding.request_resize(u32::MAX, 900));
        updates.active.set(false);
        assert!(!binding.request_resize(1200, 900));
        let mut size = ViewRect {
            left: 0,
            top: 0,
            right: 0,
            bottom: 0,
        };
        // SAFETY: This unattached owner-thread view and writable rectangle remain owned through the query.
        assert_eq!(unsafe { view.getSize(&raw mut size) }, kResultOk);
        assert_eq!((size.right, size.bottom), EDITOR_SIZE);
    }
}
