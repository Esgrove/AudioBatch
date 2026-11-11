//! Independent worker lifecycle tests never inspect or create normal user preset storage.

use std::cell::RefCell;
use std::rc::Rc;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};

use super::PresetEditor;
use crate::params::{EditorBinding, EditorShutdown};

#[derive(Default)]
struct Binding(RefCell<Vec<EditorShutdown>>);

impl EditorBinding for Binding {
    fn begin_edit(&self, _: u32) {}
    fn set_value(&self, _: u32, _: f64) {}
    fn end_edit(&self, _: u32) {}
    fn register_shutdown(&self, shutdown: EditorShutdown) {
        self.0.borrow_mut().push(shutdown);
    }
}

#[test]
fn independent_native_shutdown_joins_file_worker_before_retained_program_drop() {
    let binding = Rc::new(Binding::default());
    let editor = PresetEditor::with_binding(binding.clone());
    let done = Arc::new(AtomicBool::new(false));
    let finished = Arc::clone(&done);
    *editor.worker.lock().expect("worker slot") = Some(std::thread::spawn(move || {
        finished.store(true, Ordering::Release);
    }));
    binding.register_shutdown(editor.shutdown());
    for shutdown in binding.0.borrow_mut().drain(..) {
        shutdown.shutdown();
    }
    assert!(done.load(Ordering::Acquire));
    assert!(editor.worker.lock().expect("joined worker").is_none());
    drop(editor);
}

#[test]
fn disconnected_worker_is_reported_and_does_not_leave_busy_state_forever() {
    let mut editor = PresetEditor::default();
    let (sender, receiver) = std::sync::mpsc::channel();
    editor.receiver = Some(receiver);
    drop(sender);
    assert!(editor.poll().is_none());
    assert!(!editor.busy());
    assert!(editor.error.is_some());
}
