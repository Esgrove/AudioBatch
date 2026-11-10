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
/// Bounds unreasonable host dimensions without overflow.
const MAXIMUM_DIMENSION: i32 = 4096;

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
        // SAFETY: VST3 guarantees a live parent until removed, and this editor's methods and drop stay on its UI owner.
        let Ok(editor) = (unsafe {
            Editor::attach(
                parent,
                width as u32,
                height as u32,
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
        if !(MINIMUM_SIZE.0..=MAXIMUM_DIMENSION).contains(&width)
            || !(MINIMUM_SIZE.1..=MAXIMUM_DIMENSION).contains(&height)
        {
            return kResultFalse;
        }
        if self.updates.resize(width as u32, height as u32).is_err() {
            return kResultFalse;
        }
        self.dimensions.set((width, height));
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
        let width = rect
            .right
            .saturating_sub(rect.left)
            .clamp(MINIMUM_SIZE.0, MAXIMUM_DIMENSION);
        let height = rect
            .bottom
            .saturating_sub(rect.top)
            .clamp(MINIMUM_SIZE.1, MAXIMUM_DIMENSION);
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
        if !(MINIMUM_SIZE.0..=MAXIMUM_DIMENSION).contains(&width)
            || !(MINIMUM_SIZE.1..=MAXIMUM_DIMENSION).contains(&height)
        {
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
        unsafe { frame.resizeView(view, &raw mut size) == kResultOk }
    }
}
