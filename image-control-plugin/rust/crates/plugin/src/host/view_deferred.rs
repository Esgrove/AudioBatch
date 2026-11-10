//! Coalesced native editor changes applied outside synchronous Iced-to-host callbacks.

use std::cell::{Cell, RefCell};
use std::rc::Rc;

use crate::deferred_ui::Deferred;
use crate::iced_bridge::Editor;

/// UI-owned native resources, with one cancellable next-turn operation rather than reentrant native calls.
pub(super) struct ViewUpdates {
    /// Native editor ownership is shared with the COM view, not with DSP or worker threads.
    pub(super) editor: RefCell<Option<Rc<Editor>>>,
    /// Host callbacks may run nested event loops, so a timer alone does not establish a safe turn.
    callback_depth: Cell<usize>,
    /// Native geometry notifications can reenter host methods while the native window is borrowed.
    flushing: Cell<bool>,
    /// Latest accepted native size replaces earlier pending requests.
    size: Cell<Option<(u32, u32)>>,
    /// Latest accepted native fallback scale.
    scale: Cell<Option<f64>>,
    /// True only for a successfully attached, not-yet-removed child.
    pub(super) active: Cell<bool>,
    /// Native timer is cancelled before this state or its window can disappear.
    deferred: Deferred,
}

/// Keeps callback depth balanced across every return path, without retaining any native window borrow.
pub(super) struct HostCall(Rc<ViewUpdates>);

impl ViewUpdates {
    /// Installs a weak timer callback, avoiding an editor-to-program-to-view ownership cycle.
    pub(super) fn new() -> Rc<Self> {
        Rc::new_cyclic(|weak: &std::rc::Weak<Self>| {
            let weak = weak.clone();
            Self {
                editor: RefCell::new(None),
                callback_depth: Cell::new(0),
                flushing: Cell::new(false),
                size: Cell::new(None),
                scale: Cell::new(None),
                active: Cell::new(false),
                deferred: Deferred::new(move || {
                    if let Some(updates) = weak.upgrade() {
                        updates.flush();
                    }
                }),
            }
        })
    }

    /// Defers host geometry even when a nested host event loop runs before Iced finishes its update.
    pub(super) fn resize(&self, width: u32, height: u32) -> std::io::Result<()> {
        if !self.active.get() {
            return Ok(());
        }
        self.deferred.schedule()?;
        self.size.set(Some((width, height)));
        Ok(())
    }

    /// Stores fallback DPI without synchronously entering the native resize handler.
    pub(super) fn scale(&self, factor: f64) -> std::io::Result<()> {
        if !self.active.get() {
            return Ok(());
        }
        self.deferred.schedule()?;
        self.scale.set(Some(factor));
        Ok(())
    }

    /// Marks removal before ending gestures, preventing any later user edit or resize from reviving the view.
    pub(super) fn remove(self: &Rc<Self>) -> Result<(), crate::iced_bridge::EditorError> {
        if !self.active.get() && self.editor.borrow().is_none() {
            return Ok(());
        }
        removal_admission(cfg!(windows), self.callback_depth.get(), self.flushing.get())?;
        self.active.set(false);
        self.size.set(None);
        self.scale.set(None);
        self.deferred.cancel();
        let editor = self.editor.borrow_mut().take();
        editor.map_or(Ok(()), |editor| editor.close())
    }

    /// Captures host-call ownership while no `RefCell` borrow or native resize is active.
    pub(super) fn host_call(self: &Rc<Self>) -> HostCall {
        self.callback_depth.set(self.callback_depth.get() + 1);
        HostCall(Rc::clone(self))
    }

    /// Runs on the owner thread after the host callback and its surrounding Iced update have unwound.
    fn flush(&self) {
        if self.callback_depth.get() != 0 || self.flushing.get() {
            let _ = self.deferred.schedule();
            return;
        }
        if !self.active.get() {
            return;
        }
        self.flushing.set(true);
        let scale = self.scale.take();
        let size = self.size.take();
        // Native resize can synchronously notify Iced.
        // Host calls made by that update only write pending fields and schedule another turn,
        // never enter this editor again.
        let editor = self.editor.borrow().clone();
        if let Some(editor) = editor {
            if let Some(scale) = scale {
                let _ = editor.set_scale_factor(scale);
            }
            if self.active.get()
                && let Some((width, height)) = size
            {
                let _ = editor.resize(width, height);
            }
        }
        self.flushing.set(false);
    }
}

/// Hiding or reparenting a focused Win32 child can synchronously deliver focus/DPI messages into Iced.
/// Reject before changing any lifecycle or pending work, rather than claiming successful asynchronous removal.
const fn removal_admission(
    windows: bool,
    callback_depth: usize,
    flushing: bool,
) -> Result<(), crate::iced_bridge::EditorError> {
    if windows && (callback_depth != 0 || flushing) {
        Err(crate::iced_bridge::EditorError::CallbackActive)
    } else {
        Ok(())
    }
}

impl Drop for HostCall {
    /// Scheduling remains deferred after the final host return, rather than flushing inside the Iced update.
    fn drop(&mut self) {
        self.0.callback_depth.set(self.0.callback_depth.get() - 1);
    }
}

impl Drop for ViewUpdates {
    /// Final COM release cancels all native timers before dropping editor resources.
    fn drop(&mut self) {
        self.active.set(false);
        self.deferred.cancel();
    }
}

#[cfg(test)]
mod tests {
    use super::{ViewUpdates, removal_admission};
    #[cfg(not(windows))]
    use std::rc::Rc;

    #[test]
    fn win32_removal_requires_an_unborrowed_ui_turn() {
        for (depth, flushing) in [(1, false), (0, true), (2, true)] {
            assert!(matches!(
                removal_admission(true, depth, flushing),
                Err(crate::iced_bridge::EditorError::CallbackActive)
            ));
            assert!(removal_admission(false, depth, flushing).is_ok());
        }
        assert!(removal_admission(true, 0, false).is_ok());
    }

    #[test]
    #[cfg(windows)]
    fn rejected_win32_removal_preserves_ownership_and_pending_geometry_until_retry() {
        let updates = ViewUpdates::new();
        updates.active.set(true);
        updates.size.set(Some((900, 700)));
        let call = updates.host_call();
        assert!(updates.remove().is_err());
        assert!(updates.active.get());
        assert_eq!(updates.size.get(), Some((900, 700)));
        drop(call);
        updates.flushing.set(true);
        assert!(updates.remove().is_err());
        assert!(updates.active.get());
        updates.flushing.set(false);
        updates.remove().expect("retry outside Iced callback");
        assert!(!updates.active.get());
        assert_eq!(updates.size.get(), None);
    }

    #[test]
    fn nested_host_calls_keep_geometry_pending_until_the_outer_callback_returns() {
        let updates = ViewUpdates::new();
        updates.active.set(true);
        updates.size.set(Some((900, 700)));
        updates.scale.set(Some(2.0));
        let outer = updates.host_call();
        let inner = updates.host_call();
        updates.flush();
        assert_eq!(updates.size.get(), Some((900, 700)));
        drop(inner);
        updates.flush();
        assert_eq!(updates.scale.get(), Some(2.0));
        drop(outer);
        updates.flush();
        assert_eq!(updates.size.get(), None);
        assert_eq!(updates.scale.get(), None);
    }

    #[test]
    #[cfg(not(windows))]
    fn reentrant_host_removal_cancels_geometry_synchronously() {
        let updates = ViewUpdates::new();
        updates.active.set(true);
        let weak = Rc::downgrade(&updates);
        let call = updates.host_call();
        updates.size.set(Some((900, 700)));
        updates.remove().expect("synchronous removal");
        assert!(!updates.active.get());
        assert_eq!(updates.size.get(), None);
        drop(updates);
        drop(call);
        assert!(weak.upgrade().is_none());
    }

    #[test]
    #[cfg(not(windows))]
    fn native_flush_does_not_postpone_removal_to_another_ui_turn() {
        let updates = ViewUpdates::new();
        updates.active.set(true);
        updates.flushing.set(true);
        updates.size.set(Some((900, 700)));
        updates.remove().expect("synchronous removal");
        assert!(!updates.active.get());
        assert_eq!(updates.size.get(), None);
        assert!(updates.editor.borrow().is_none());
        updates.remove().expect("synchronous removal");
        updates.flushing.set(false);
    }
}
