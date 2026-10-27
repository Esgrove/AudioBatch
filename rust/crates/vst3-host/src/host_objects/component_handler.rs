//! The component handler that receives parameter edits and restart requests from a plugin's controller.
//!
//! A controller reports edits made in its editor through `performEdit`, usually on the UI thread,
//! but some plugins call it from other threads too.
//! Edits go into a lock-free queue that the processor handle drains into the next `process` call,
//! so the processor sees the same values as the controller.
//! Restart requests are collected as flags for the owner of the instance to act on.

#![expect(unsafe_code, reason = "implements the IComponentHandler COM interfaces")]

use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicI32, Ordering};

use crossbeam_queue::ArrayQueue;
use vst3::Class;
use vst3::Steinberg::Vst::RestartFlags_;
use vst3::Steinberg::Vst::{
    IComponentHandler, IComponentHandler2, IComponentHandler2Trait, IComponentHandlerTrait, ParamID, ParamValue,
};
use vst3::Steinberg::{FIDString, TBool, kResultFalse, kResultOk, tresult};

/// How many parameter edits can wait for the next `process` call.
/// When the queue is full the oldest edit is dropped, because a newer value supersedes it.
pub const EDIT_QUEUE_CAPACITY: usize = 1024;

/// One parameter change made in a plugin's controller.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct ParameterEdit {
    /// Parameter id.
    pub id: u32,
    /// New normalized value between 0 and 1.
    pub value: f64,
}

/// Restart requests a plugin made through `restartComponent`.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct RestartFlags(pub i32);

/// The component handler given to a plugin's controller.
pub struct ComponentHandler {
    /// Edits waiting for the processor.
    edits: Arc<ArrayQueue<ParameterEdit>>,
    /// Restart flags requested since they were last taken.
    restart_flags: AtomicI32,
    /// Whether the plugin changed since this was last cleared.
    dirty: AtomicBool,
}

impl RestartFlags {
    /// Returns true when no restart was requested.
    #[must_use]
    pub const fn is_empty(self) -> bool {
        self.0 == 0
    }

    /// Returns true when the plugin asked to be reloaded completely.
    #[must_use]
    pub fn reload_component(self) -> bool {
        self.has(RestartFlags_::kReloadComponent)
    }

    /// Returns true when the bus layout changed.
    #[must_use]
    pub fn io_changed(self) -> bool {
        self.has(RestartFlags_::kIoChanged)
    }

    /// Returns true when parameter values changed outside an edit, such as after loading a preset.
    #[must_use]
    pub fn parameter_values_changed(self) -> bool {
        self.has(RestartFlags_::kParamValuesChanged)
    }

    /// Returns true when the latency changed.
    #[must_use]
    pub fn latency_changed(self) -> bool {
        self.has(RestartFlags_::kLatencyChanged)
    }

    /// Returns true when the processor needs to be set up again, which needs a reload, a bus or a latency change.
    #[must_use]
    pub fn needs_reconfiguration(self) -> bool {
        self.reload_component() || self.io_changed() || self.latency_changed()
    }

    /// Returns true when the flag bit is set.
    fn has(self, flag: impl Into<i64>) -> bool {
        i64::from(self.0) & flag.into() != 0
    }
}

impl Class for ComponentHandler {
    type Interfaces = (IComponentHandler, IComponentHandler2);
}

impl ComponentHandler {
    /// Creates a handler that queues edits into `edits`.
    pub const fn new(edits: Arc<ArrayQueue<ParameterEdit>>) -> Self {
        Self {
            edits,
            restart_flags: AtomicI32::new(0),
            dirty: AtomicBool::new(false),
        }
    }

    /// Returns and clears the restart flags requested so far.
    pub fn take_restart_flags(&self) -> RestartFlags {
        RestartFlags(self.restart_flags.swap(0, Ordering::AcqRel))
    }

    /// Returns and clears whether the plugin reported a change.
    #[cfg(test)]
    pub fn take_dirty(&self) -> bool {
        self.dirty.swap(false, Ordering::AcqRel)
    }
}

impl IComponentHandlerTrait for ComponentHandler {
    unsafe fn beginEdit(&self, _id: ParamID) -> tresult {
        kResultOk
    }

    unsafe fn performEdit(&self, id: ParamID, value_normalized: ParamValue) -> tresult {
        self.edits.force_push(ParameterEdit {
            id,
            value: value_normalized,
        });
        self.dirty.store(true, Ordering::Release);
        kResultOk
    }

    unsafe fn endEdit(&self, _id: ParamID) -> tresult {
        kResultOk
    }

    unsafe fn restartComponent(&self, flags: i32) -> tresult {
        self.restart_flags.fetch_or(flags, Ordering::AcqRel);
        if RestartFlags(flags).parameter_values_changed() {
            self.dirty.store(true, Ordering::Release);
        }
        kResultOk
    }
}

impl IComponentHandler2Trait for ComponentHandler {
    unsafe fn setDirty(&self, state: TBool) -> tresult {
        if state != 0 {
            self.dirty.store(true, Ordering::Release);
        }
        kResultOk
    }

    unsafe fn requestOpenEditor(&self, _name: FIDString) -> tresult {
        kResultFalse
    }

    unsafe fn startGroupEdit(&self) -> tresult {
        kResultOk
    }

    unsafe fn finishGroupEdit(&self) -> tresult {
        kResultOk
    }
}

#[cfg(test)]
mod test_component_handler {
    use super::*;

    /// Returns a restart flag as the argument type of `restartComponent`.
    fn flag_bits(flag: impl Into<i64>) -> i32 {
        i32::try_from(flag.into()).expect("restart flags fit in 32 bits")
    }

    #[test]
    fn edits_are_queued_and_mark_the_plugin_dirty() {
        let edits = Arc::new(ArrayQueue::new(4));
        let handler = ComponentHandler::new(Arc::clone(&edits));
        // SAFETY: the handler methods only read their plain arguments.
        unsafe {
            handler.beginEdit(3);
            handler.performEdit(3, 0.25);
            handler.endEdit(3);
        }
        assert_eq!(edits.pop(), Some(ParameterEdit { id: 3, value: 0.25 }));
        assert!(handler.take_dirty());
        assert!(!handler.take_dirty());
    }

    #[test]
    fn a_full_queue_drops_the_oldest_edit() {
        let edits = Arc::new(ArrayQueue::new(2));
        let handler = ComponentHandler::new(Arc::clone(&edits));
        // SAFETY: the handler methods only read their plain arguments.
        unsafe {
            handler.performEdit(1, 0.1);
            handler.performEdit(2, 0.2);
            handler.performEdit(3, 0.3);
        }
        assert_eq!(edits.pop().map(|edit| edit.id), Some(2));
        assert_eq!(edits.pop().map(|edit| edit.id), Some(3));
    }

    #[test]
    fn restart_flags_accumulate_until_taken() {
        let handler = ComponentHandler::new(Arc::new(ArrayQueue::new(1)));
        // SAFETY: the handler methods only read their plain arguments.
        unsafe {
            handler.restartComponent(flag_bits(RestartFlags_::kLatencyChanged));
            handler.restartComponent(flag_bits(RestartFlags_::kParamValuesChanged));
        }
        let flags = handler.take_restart_flags();
        assert!(flags.latency_changed());
        assert!(flags.parameter_values_changed());
        assert!(flags.needs_reconfiguration());
        assert!(!flags.io_changed());
        assert!(handler.take_restart_flags().is_empty());
        assert!(handler.take_dirty());
    }
}
