//! The plug frame through which an editor view asks its window for a new size.
//!
//! The frame holds only weak references to the view and the window,
//! so a plugin that keeps the frame after the editor closed cannot keep either alive.
//! Weak reference counts are touched and dropped only on the UI thread,
//! even if a plugin releases its last `IPlugFrame` reference from a worker thread.

#![expect(unsafe_code, reason = "implements the IPlugFrame COM interface")]

use std::mem::ManuallyDrop;
use std::rc::Weak;

use vst3::Class;
use vst3::Steinberg::{
    IPlugFrame, IPlugFrameTrait, IPlugView, ViewRect, kInvalidArgument, kResultFalse, kResultTrue, tresult,
};

use super::{EditorShared, NativeWindow, view_size};
#[cfg(test)]
use super::{ViewSize, view_rect};
use crate::thread::{UiDispatcher, UiThread};

/// The frame given to an editor view.
pub struct PlugFrame {
    /// UI-thread-bound weak handles to the editor state and native window.
    targets: ManuallyDrop<FrameTargets>,
    /// Dispatches off-thread COM release teardown back to the UI thread.
    dispatcher: Option<UiDispatcher>,
}

/// Non-atomic weak handles owned exclusively by the UI thread.
struct FrameTargets {
    /// The view and its flags.
    shared: Weak<EditorShared>,
    /// The window showing the view.
    window: Weak<NativeWindow>,
}

/// Carries UI-bound weak handles to the UI thread for destruction, leaking if the UI loop stopped.
struct DropTargetsOnUi(ManuallyDrop<FrameTargets>);

#[expect(
    clippy::non_send_fields_in_send_ty,
    reason = "targets are accessed only with a UiThread token and dropped on the UI thread or leaked"
)]
// SAFETY: `targets` is dereferenced only after `UiThread::current()` succeeds,
// and `Drop` either runs on the UI thread, posts `DropTargetsOnUi` to it, or leaks `targets`.
unsafe impl Send for PlugFrame {}

// SAFETY: Shared references only read `targets` on the UI thread after `UiThread::current()` succeeds.
unsafe impl Sync for PlugFrame {}

#[expect(
    clippy::non_send_fields_in_send_ty,
    reason = "the carrier only moves the weak handles to the UI thread to drop them there"
)]
// SAFETY: `FrameTargets` is never dereferenced off the UI thread and is dropped only when `UiThread::is_current()`.
unsafe impl Send for DropTargetsOnUi {}

impl PlugFrame {
    /// Creates the frame of a view shown in a window.
    pub(super) const fn new(
        shared: Weak<EditorShared>,
        window: Weak<NativeWindow>,
        dispatcher: Option<UiDispatcher>,
    ) -> Self {
        Self {
            targets: ManuallyDrop::new(FrameTargets { shared, window }),
            dispatcher,
        }
    }
}

impl Class for PlugFrame {
    type Interfaces = (IPlugFrame,);
}

impl IPlugFrameTrait for PlugFrame {
    unsafe fn resizeView(&self, view: *mut IPlugView, new_size: *mut ViewRect) -> tresult {
        if new_size.is_null() {
            return kInvalidArgument;
        }
        let Some(_ui) = UiThread::current() else {
            return kResultFalse;
        };
        let (Some(shared), Some(window)) = (self.targets.shared.upgrade(), self.targets.window.upgrade()) else {
            return kResultFalse;
        };
        if view != shared.view.as_ptr() {
            return kInvalidArgument;
        }
        // SAFETY: the plugin passes a valid rectangle, checked for null above.
        let size = view_size(unsafe { &*new_size });
        if size.width == 0 || size.height == 0 {
            return kInvalidArgument;
        }
        shared.resizing_for_plugin.set(true);
        window.set_content_size(size);
        shared.resizing_for_plugin.set(false);
        shared.tell_size(size);
        kResultTrue
    }
}

impl Drop for PlugFrame {
    fn drop(&mut self) {
        // SAFETY: `targets` is taken once here and never accessed again.
        let targets = unsafe { ManuallyDrop::take(&mut self.targets) };
        if UiThread::is_current() {
            drop(targets);
            return;
        }
        let carrier = DropTargetsOnUi(ManuallyDrop::new(targets));
        if let Some(dispatcher) = self.dispatcher.take() {
            dispatcher.post(move |_| drop(carrier));
        }
    }
}

impl Drop for DropTargetsOnUi {
    fn drop(&mut self) {
        if UiThread::is_current() {
            // SAFETY: taken once on the UI thread.
            // Leaked if the dispatcher dropped the job on shutdown.
            drop(unsafe { ManuallyDrop::take(&mut self.0) });
        }
    }
}

#[cfg(test)]
mod test_plug_frame {
    use std::rc::Rc;

    use vst3::ComWrapper;

    use super::*;
    use crate::test_helpers::{dispatcher, on_ui};

    #[test]
    fn requests_are_refused_once_the_editor_is_gone_or_off_ui_thread() {
        let mut rect = view_rect(ViewSize {
            width: 300,
            height: 200,
        });
        on_ui(move |_| {
            let frame = PlugFrame::new(Weak::new(), Weak::new(), None);
            // SAFETY: the rectangle lives for the call, and the view pointer is only compared.
            unsafe {
                assert_eq!(
                    frame.resizeView(std::ptr::null_mut(), std::ptr::null_mut()),
                    kInvalidArgument
                );
                assert_eq!(frame.resizeView(std::ptr::null_mut(), &raw mut rect), kResultFalse);
            }
        });
    }

    #[test]
    fn off_thread_com_release_dispatches_weak_drop_to_ui_thread() {
        let ui_dispatcher = dispatcher();
        let wrapper = ui_dispatcher
            .call(
                |_| {
                    let anchor = Rc::new(());
                    let _weak = Rc::downgrade(&anchor);
                    ComWrapper::new(PlugFrame::new(Weak::new(), Weak::new(), Some(dispatcher().clone())))
                },
                &|| false,
            )
            .expect("creates plug frame on UI thread");
        std::thread::spawn(move || drop(wrapper))
            .join()
            .expect("drops wrapper off UI thread");
        ui_dispatcher
            .call(|_| (), &|| false)
            .expect("drains queued UI teardown");
    }
}
