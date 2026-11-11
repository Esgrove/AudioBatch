//! Independent worker and command lifecycle tests never inspect or create normal user preset storage.

use std::cell::{Cell, RefCell};
use std::rc::Rc;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};

use super::{Action, Completion, Outcome, PresetEditor};
use crate::editing::SoundSnapshot;
use crate::editor::{EditorState, Message};
use crate::params::{EditorBinding, EditorShutdown, GAIN_PARAMETER, Parameters, Snapshot};

#[derive(Default)]
struct Binding {
    shutdowns: RefCell<Vec<EditorShutdown>>,
    close_on_register: Cell<bool>,
    close_on_dirty: Cell<bool>,
    close_on_end: Cell<bool>,
    closed: Cell<bool>,
    restore_on_end: RefCell<Option<(Arc<Parameters>, Snapshot)>>,
    writes: Cell<usize>,
}

impl EditorBinding for Binding {
    fn is_open(&self) -> bool {
        !self.closed.get()
    }
    fn begin_edit(&self, _: u32) {}
    fn set_value(&self, _: u32, _: f64) {
        self.writes.set(self.writes.get() + 1);
    }
    fn end_edit(&self, _: u32) {
        let restore = self.restore_on_end.borrow_mut().take();
        if let Some((parameters, snapshot)) = restore {
            assert!(parameters.restore(snapshot));
        }
        if self.close_on_end.get() {
            self.retire_workers();
        }
    }
    fn register_shutdown(&self, shutdown: EditorShutdown) {
        self.shutdowns.borrow_mut().push(shutdown);
        if self.close_on_register.get() {
            self.retire_workers();
        }
    }
    fn mark_dirty(&self) {
        if self.close_on_dirty.get() {
            self.retire_workers();
        }
    }
}

impl Binding {
    /// Mirrors native close's independent worker drain without destroying the retained program.
    fn retire_workers(&self) {
        self.closed.set(true);
        let shutdowns = std::mem::take(&mut *self.shutdowns.borrow_mut());
        for shutdown in shutdowns {
            shutdown.shutdown();
        }
    }
}

/// Constructs the production update owner with independent worker retirement but no native window or GPU.
fn editor() -> (EditorState, Rc<Binding>) {
    let binding = Rc::new(Binding::default());
    let mut state = EditorState::for_host_test(
        Arc::new(Parameters::new()),
        image_control_dsp::Imager::new().shared(),
        binding.clone(),
    );
    state.presets = PresetEditor::with_binding(binding.clone());
    (state, binding)
}

#[test]
fn independent_native_shutdown_joins_file_worker_before_retained_program_drop() {
    let binding = Rc::new(Binding::default());
    let mut editor = PresetEditor::with_binding(binding.clone());
    let done = Arc::new(AtomicBool::new(false));
    let finished = Arc::clone(&done);
    assert!(
        editor
            .worker
            .spawn(move || finished.store(true, Ordering::Release))
            .expect("worker")
    );
    binding.retire_workers();
    assert!(done.load(Ordering::Acquire));
    assert!(editor.worker.is_closed());
    // Permanent admission cannot reopen even if a late binding incorrectly reports itself open.
    binding.closed.set(false);
    editor.start(Action::Refresh, 0, 0, SoundSnapshot::capture(Snapshot::default()), None);
    assert!(!editor.busy());
    assert!(
        !editor
            .worker
            .spawn(|| panic!("retired worker ran"))
            .expect("closed admission")
    );
    drop(editor);
}

#[test]
fn registration_reentry_closes_admission_before_first_file_operation() {
    let binding = Rc::new(Binding {
        close_on_register: Cell::new(true),
        ..Binding::default()
    });
    let mut editor = PresetEditor::with_binding(binding.clone());
    assert!(editor.worker.is_closed());
    binding.closed.set(false);
    editor.start(Action::Refresh, 0, 0, SoundSnapshot::capture(Snapshot::default()), None);
    assert!(!editor.busy());
}

#[test]
fn completed_operation_join_keeps_admission_open_until_native_retirement() {
    let editor = PresetEditor::default();
    for _ in 0..2 {
        assert!(editor.worker.spawn(|| {}).expect("admitted operation"));
        editor.worker.finish();
        assert!(!editor.worker.is_closed());
    }
    editor.worker.shutdown();
    assert!(
        !editor
            .worker
            .spawn(|| panic!("retired operation ran"))
            .expect("closed admission")
    );
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
    assert!(!editor.worker.is_closed());
}

#[test]
fn restore_during_gesture_end_cancels_following_sound_and_file_commands() {
    for command in [
        Message::Compare(1),
        Message::CopyComparison,
        Message::Undo,
        Message::Redo,
        Message::PresetAction(Action::Refresh),
        Message::Presets,
    ] {
        let (mut state, binding) = editor();
        let _ = state.update(Message::Change(GAIN_PARAMETER, 0.75));
        let restored = Snapshot {
            gain: 0.25,
            ..Snapshot::default()
        };
        *binding.restore_on_end.borrow_mut() = Some((Arc::clone(&state.parameters), restored));
        let writes = binding.writes.get();
        let _ = state.update(command);
        assert_eq!(state.parameters.snapshot().gain, restored.gain);
        assert_eq!(binding.writes.get(), writes);
        state.parameters.with_editing(|editing, snapshot| {
            let comparison = editing.comparison(snapshot);
            assert_eq!(comparison.active, 0);
            assert_eq!(comparison.slots, [SoundSnapshot::capture(restored); 2]);
            assert!(!editing.can_undo());
        });
        assert!(!state.presets.busy());
        assert!(!state.presets.shown);
    }
}

#[test]
fn close_during_gesture_end_cancels_comparison_and_followup_file_admission() {
    for command in [Message::Compare(1), Message::PresetAction(Action::Refresh)] {
        let (mut state, binding) = editor();
        let _ = state.update(Message::Change(GAIN_PARAMETER, 0.75));
        binding.close_on_end.set(true);
        let writes = binding.writes.get();
        let _ = state.update(command);
        assert_eq!(state.parameters.get(GAIN_PARAMETER), Some(0.75));
        assert_eq!(binding.writes.get(), writes);
        assert_eq!(
            state
                .parameters
                .with_editing(|editing, snapshot| editing.comparison(snapshot))
                .active,
            0
        );
        assert!(state.presets.worker.is_closed());
        assert!(!state.presets.busy());
    }
}

#[test]
fn restore_during_numeric_commit_cancels_following_comparison_command() {
    let (mut state, binding) = editor();
    let _ = state.update(Message::NumericFocus(GAIN_PARAMETER));
    let _ = state.update(Message::NumericDraft(GAIN_PARAMETER, "6 dB".into()));
    let restored = Snapshot {
        gain: 0.25,
        ..Snapshot::default()
    };
    *binding.restore_on_end.borrow_mut() = Some((Arc::clone(&state.parameters), restored));
    let _ = state.update(Message::Compare(1));
    assert_eq!(state.parameters.snapshot().gain, restored.gain);
    assert_eq!(
        state
            .parameters
            .with_editing(|editing, snapshot| editing.comparison(snapshot))
            .active,
        0
    );
}

#[test]
fn saved_completion_dirty_close_cannot_spawn_followup_refresh() {
    let directory = tempfile::tempdir().expect("isolated library");
    let library = crate::user_presets::PresetLibrary::new(directory.path().canonicalize().expect("physical library"));
    let (mut state, binding) = editor();
    let sound = SoundSnapshot::capture(state.parameters.snapshot());
    let preset = library.create("Saved sound", sound).expect("isolated preset");
    let (sender, receiver) = std::sync::mpsc::channel();
    state.presets.receiver = Some(receiver);
    assert!(
        sender
            .send(Completion {
                generation: state.parameters.restore_generation(),
                epoch: state.presets.epoch,
                slot: 0,
                before: sound,
                result: Ok(Outcome::Saved(preset)),
            })
            .is_ok()
    );
    binding.close_on_dirty.set(true);
    let _ = state.update(Message::Refresh);
    assert!(binding.closed.get());
    assert!(state.presets.worker.is_closed());
    assert!(!state.presets.busy());
    assert!(binding.shutdowns.borrow().is_empty());
    // The already accepted save metadata remains valid. Close blocks only the follow-up file operation.
    assert_eq!(
        state
            .parameters
            .with_editing(|editing, snapshot| editing.comparison(snapshot))
            .origins[0]
            .as_ref()
            .expect("accepted save origin")
            .name,
        "Saved sound"
    );
}
