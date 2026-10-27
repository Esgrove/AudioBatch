//! The plug frame through which an editor view asks its window for a new size.
//!
//! The frame holds only weak references to the view and the window,
//! so a plugin that keeps the frame after the editor closed cannot keep either alive.

#![expect(unsafe_code, reason = "implements the IPlugFrame COM interface")]

use std::rc::Weak;

use vst3::Class;
use vst3::Steinberg::{
    IPlugFrame, IPlugFrameTrait, IPlugView, ViewRect, kInvalidArgument, kResultFalse, kResultTrue, tresult,
};

use super::{EditorShared, NativeWindow, ViewSize};

/// The frame given to an editor view.
pub struct PlugFrame {
    /// The view and its flags.
    shared: Weak<EditorShared>,
    /// The window showing the view.
    window: Weak<NativeWindow>,
}

impl Class for PlugFrame {
    type Interfaces = (IPlugFrame,);
}

impl PlugFrame {
    /// Creates the frame of a view shown in a window.
    pub(super) const fn new(shared: Weak<EditorShared>, window: Weak<NativeWindow>) -> Self {
        Self { shared, window }
    }
}

impl IPlugFrameTrait for PlugFrame {
    unsafe fn resizeView(&self, view: *mut IPlugView, new_size: *mut ViewRect) -> tresult {
        if new_size.is_null() {
            return kInvalidArgument;
        }
        let (Some(shared), Some(window)) = (self.shared.upgrade(), self.window.upgrade()) else {
            return kResultFalse;
        };
        if view != shared.view.as_ptr() {
            return kInvalidArgument;
        }
        // SAFETY: the plugin passes a valid rectangle, checked for null above.
        let size = ViewSize::from_rect(unsafe { &*new_size });
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

#[cfg(test)]
mod test_plug_frame {
    use super::*;

    #[test]
    fn requests_are_refused_once_the_editor_is_gone() {
        let frame = PlugFrame::new(Weak::new(), Weak::new());
        let mut rect = ViewSize {
            width: 300,
            height: 200,
        }
        .rect();
        // SAFETY: the rectangle lives for the call, and the view pointer is only compared.
        unsafe {
            assert_eq!(
                frame.resizeView(std::ptr::null_mut(), std::ptr::null_mut()),
                kInvalidArgument
            );
            assert_eq!(frame.resizeView(std::ptr::null_mut(), &raw mut rect), kResultFalse);
        }
    }
}
