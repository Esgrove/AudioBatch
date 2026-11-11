//! Hardware-free contracts for production editor commands, retained widgets, and host gesture ownership.

use std::cell::RefCell;

use iced::widget::Space;
use iced::{Rectangle, Theme};
use image_control_dsp::{AnalysisStatus, CorrelationSnapshot};

use super::chrome::{Chrome, Symbol};
use super::controls::{analysis_status_text, control_input, parameter_input};
use super::*;
use crate::params::{GAIN_PARAMETER, MONO_PARAMETER};

/// Local history survives editor replacement, while automation alone remains outside it.
#[test]
fn local_history_replays_only_touched_values_and_survives_editor_replacement() {
    let (mut state, binding) = editor();
    let original = state.parameters.get(GAIN_PARAMETER);
    let _ = state.update(Message::Change(GAIN_PARAMETER, 0.25));
    let _ = state.update(Message::EndGesture);
    state.parameters.set(MONO_PARAMETER, 1.0);
    let parameters = Arc::clone(&state.parameters);
    let shared = Arc::clone(&state.shared);
    drop(state);
    let mut reopened = EditorState::for_host_test(parameters, shared, binding);
    let _ = reopened.update(Message::Undo);
    assert_eq!(reopened.parameters.get(GAIN_PARAMETER), original);
    assert_eq!(reopened.parameters.get(MONO_PARAMETER), Some(1.0));
    let _ = reopened.update(Message::Redo);
    assert_eq!(reopened.parameters.get(GAIN_PARAMETER), Some(0.25));
}

/// A/B preserves live audition and Link state and records switch/copy alongside local sound edits.
#[test]
fn comparisons_are_sound_only_and_metadata_copy_is_undoable() {
    let (mut state, _) = editor();
    state.parameters.set(GAIN_PARAMETER, 0.25);
    state.parameters.set(BYPASS_PARAMETER, 1.0);
    state.parameters.set(DELTA_PARAMETER, 1.0);
    state.parameters.set(9, 1.0);
    let _ = state.update(Message::Compare(1));
    assert_eq!(state.parameters.get(GAIN_PARAMETER), Some(crate::params::NEUTRAL_GAIN));
    for id in [BYPASS_PARAMETER, DELTA_PARAMETER, 9] {
        assert_eq!(state.parameters.get(id), Some(1.0));
    }
    let _ = state.update(Message::Undo);
    assert_eq!(state.parameters.get(GAIN_PARAMETER), Some(0.25));
    assert_eq!(
        state
            .parameters
            .with_editing(|editing, snapshot| editing.comparison(snapshot))
            .active,
        0
    );
    let _ = state.update(Message::CopyComparison);
    let comparison = state
        .parameters
        .with_editing(|editing, snapshot| editing.comparison(snapshot));
    assert_eq!(comparison.slots[0], comparison.slots[1]);
    let _ = state.update(Message::Undo);
    let comparison = state
        .parameters
        .with_editing(|editing, snapshot| editing.comparison(snapshot));
    assert_ne!(comparison.slots[0], comparison.slots[1]);
}

/// Repeated replay retains accepted readback even when restoring an off-grid legacy value snaps once.
#[test]
fn repeated_history_replay_preserves_exact_accepted_grid_representation() {
    let (mut state, _) = editor();
    let mut legacy = state.parameters.snapshot();
    legacy.gain = 0.123_456_789;
    assert!(state.parameters.restore(legacy));
    let _ = state.update(Message::Refresh);
    let _ = state.update(Message::Change(GAIN_PARAMETER, 0.75));
    let _ = state.update(Message::EndGesture);
    let _ = state.update(Message::Undo);
    let accepted = state.parameters.get(GAIN_PARAMETER).expect("accepted legacy undo");
    for _ in 0..8 {
        let _ = state.update(Message::Redo);
        assert_eq!(state.parameters.get(GAIN_PARAMETER), Some(0.75));
        let _ = state.update(Message::Undo);
        assert_eq!(
            state.parameters.get(GAIN_PARAMETER).expect("undo").to_bits(),
            accepted.to_bits()
        );
    }
}

/// Records the real editor's host callbacks without creating a native host or GPU device.
#[derive(Default)]
struct RecordingBinding {
    events: RefCell<Vec<(u32, Option<f64>, bool)>>,
    resizes: RefCell<Vec<Size>>,
    closed: std::cell::Cell<bool>,
    close_on_begin: std::cell::Cell<bool>,
    dirty_notifications: std::cell::Cell<usize>,
    on_end: RefCell<Option<Box<dyn FnOnce()>>>,
}

impl EditorBinding for RecordingBinding {
    fn is_open(&self) -> bool {
        !self.closed.get()
    }
    fn begin_edit(&self, id: u32) {
        self.events.borrow_mut().push((id, None, true));
        if self.close_on_begin.get() {
            self.closed.set(true);
        }
    }
    fn set_value(&self, id: u32, value: f64) {
        self.events.borrow_mut().push((id, Some(value), false));
    }
    fn end_edit(&self, id: u32) {
        self.events.borrow_mut().push((id, None, false));
        let callback = self.on_end.borrow_mut().take();
        if let Some(callback) = callback {
            callback();
        }
    }
    fn mark_dirty(&self) {
        self.dirty_notifications.set(self.dirty_notifications.get() + 1);
    }
    fn request_resize(&self, width: u32, height: u32) -> bool {
        self.resizes.borrow_mut().push(Size::new(width as f32, height as f32));
        true
    }
}

/// Rejects automation admission while retaining observable gesture edges and attempted writes.
#[derive(Default)]
struct RejectingBinding {
    recorded: RecordingBinding,
    attempts: RefCell<Vec<(u32, f64)>>,
}

impl EditorBinding for RejectingBinding {
    fn begin_edit(&self, id: u32) {
        self.recorded.begin_edit(id);
    }
    fn set_value(&self, id: u32, value: f64) {
        self.recorded.set_value(id, value);
    }
    fn publish_value(&self, _parameters: &Parameters, id: u32, value: f64) -> bool {
        self.attempts.borrow_mut().push((id, value));
        false
    }
    fn end_edit(&self, id: u32) {
        self.recorded.end_edit(id);
    }
}

/// Exercises the ordinary editor update path without opening the editor-owned analyzer consumer.
fn editor() -> (EditorState, Rc<RecordingBinding>) {
    let binding = Rc::new(RecordingBinding::default());
    let state = EditorState {
        restore_generation: 0,
        parameters: Arc::new(Parameters::default()),
        shared: image_control_dsp::Imager::new().shared(),
        settings_shown: false,
        binding: binding.clone(),
        spectrogram: Err(std::io::Error::other("headless interaction test")),
        window_size: None,
        notifier: None,
        gesture: None,
        numeric_draft: None,
        presets: presets::PresetEditor::default(),
        linked: [false; 4],
        toolbar_shown: true,
        correlation_shown: true,
        input_held: false,
        input_cancelled: false,
        band_height: layout::BAND_HEIGHT,
        divider_drag: None,
        resize_drag: None,
        compact_bands: false,
        spectrogram_hidden: false,
        analysis_stopped: false,
        maximize_spectrogram: false,
        shortcuts_shown: false,
        plot_band: None,
        plot_gesture: None,
        refresh_rate: 0.0,
        frame_rate_started: Instant::now(),
        measured_frames: 0,
        editor_render_telemetry: None,
    };
    (state, binding)
}

/// Every user write reaches binding admission before touching authoritative sound parameters.
#[test]
fn rejected_transport_keeps_slider_numeric_and_toggle_sound_unchanged() {
    for (id, messages) in [
        (
            GAIN_PARAMETER,
            vec![Message::Change(GAIN_PARAMETER, 0.25), Message::EndGesture],
        ),
        (
            GAIN_PARAMETER,
            vec![
                Message::NumericDraft(GAIN_PARAMETER, "6 dB".into()),
                Message::NumericCommit(GAIN_PARAMETER),
            ],
        ),
        (MONO_PARAMETER, vec![Message::Toggle(MONO_PARAMETER)]),
    ] {
        let binding = Rc::new(RejectingBinding::default());
        let parameters = Arc::new(Parameters::new());
        let before = serde_json::to_vec(&parameters.snapshot()).expect("sound");
        let mut state = EditorState::for_host_test(
            Arc::clone(&parameters),
            image_control_dsp::Imager::new().shared(),
            binding.clone(),
        );
        for message in messages {
            let _ = state.update(message);
        }
        assert_eq!(serde_json::to_vec(&parameters.snapshot()).expect("sound"), before);
        assert_eq!(binding.attempts.borrow().len(), 1);
        assert_eq!(binding.attempts.borrow().first().map(|(id, _)| *id), Some(id));
        assert_eq!(*binding.recorded.events.borrow(), [(id, None, true), (id, None, false)]);
        assert!(state.gesture.is_none());
    }
}

/// Rejection applies to both captured Link participants and drop still closes their exact gestures.
#[test]
fn rejected_transport_keeps_linked_sound_unchanged_and_drop_balances_gestures() {
    let binding = Rc::new(RejectingBinding::default());
    let parameters = Arc::new(Parameters::new());
    let before = serde_json::to_vec(&parameters.snapshot()).expect("sound");
    let mid = band_parameter_id(0, 0).expect("Mid");
    let side = band_parameter_id(0, 1).expect("Side");
    let mut state = EditorState::for_host_test(
        Arc::clone(&parameters),
        image_control_dsp::Imager::new().shared(),
        binding.clone(),
    );
    let _ = state.update(Message::Link(0, true));
    let _ = state.update(Message::Change(mid, 0.75));
    assert_eq!(serde_json::to_vec(&parameters.snapshot()).expect("sound"), before);
    assert_eq!(
        binding.attempts.borrow().iter().map(|(id, _)| *id).collect::<Vec<_>>(),
        [mid, side]
    );
    drop(state);
    assert_eq!(
        *binding.recorded.events.borrow(),
        [
            (mid, None, true),
            (side, None, true),
            (side, None, false),
            (mid, None, false)
        ]
    );
    assert_eq!(serde_json::to_vec(&parameters.snapshot()).expect("sound"), before);
}

#[test]
fn unchanged_notifications_and_reverted_typing_preserve_newer_automation() {
    let (mut state, binding) = editor();
    let _ = state.update(Message::NumericFocus(GAIN_PARAMETER));
    let initial = state.numeric_draft.as_ref().expect("draft").text.clone();
    assert!(state.parameters.set(GAIN_PARAMETER, 0.25));
    let _ = state.update(Message::NumericDraft(GAIN_PARAMETER, initial.clone()));
    let _ = state.update(Message::NumericCommit(GAIN_PARAMETER));
    assert_eq!(state.parameters.get(GAIN_PARAMETER), Some(0.25));
    let _ = state.update(Message::NumericDraft(GAIN_PARAMETER, "6 dB".into()));
    let _ = state.update(Message::NumericDraft(GAIN_PARAMETER, initial));
    let _ = state.update(Message::NumericBlur(GAIN_PARAMETER));
    assert_eq!(state.parameters.get(GAIN_PARAMETER), Some(0.25));
    assert!(binding.events.borrow().is_empty());
}

#[test]
fn settings_excludes_background_numeric_edits_and_traversal() {
    let (mut state, binding) = editor();
    let _ = state.update(Message::Settings);
    let _ = state.update(Message::FocusNext(false));
    let _ = state.update(Message::NumericFocus(GAIN_PARAMETER));
    let _ = state.update(Message::NumericDraft(GAIN_PARAMETER, "6 dB".into()));
    let _ = state.update(Message::NumericCommit(GAIN_PARAMETER));
    assert!(state.numeric_draft.is_none());
    assert!(binding.events.borrow().is_empty());
}

/// Preset text fields traverse normally while the modal barrier keeps sound fields unavailable.
#[test]
fn presets_allow_modal_traversal_and_reject_background_numeric_entry() {
    let (mut state, binding) = editor();
    state.presets.shown = true;
    assert_eq!(state.update(Message::FocusNext(false)).units(), 1);
    assert_eq!(state.update(Message::FocusNext(true)).units(), 1);
    state.settings_shown = true;
    assert_eq!(state.update(Message::FocusNext(false)).units(), 1);
    for message in [
        Message::NumericFocus(GAIN_PARAMETER),
        Message::NumericDraft(GAIN_PARAMETER, "6 dB".into()),
        Message::NumericCommit(GAIN_PARAMETER),
    ] {
        let _ = state.update(message);
    }
    assert!(state.numeric_draft.is_none());
    assert!(binding.events.borrow().is_empty());
}

/// Global sound shortcuts never escape a modal dialog or an unfinished parameter text draft.
#[test]
fn sound_shortcuts_leave_modal_dialogs_and_text_drafts_unchanged() {
    for context in 0..3 {
        let (mut state, binding) = editor();
        let _ = state.update(Message::Change(GAIN_PARAMETER, 0.25));
        let _ = state.update(Message::EndGesture);
        state.plot_band = Some(0);
        match context {
            0 => state.settings_shown = true,
            1 => state.presets.shown = true,
            _ => {
                let _ = state.update(Message::NumericDraft(GAIN_PARAMETER, "6 dB".into()));
            }
        }
        let before = serde_json::to_vec(&state.parameters.snapshot()).expect("sound");
        let callback_count = binding.events.borrow().len();
        for command in [
            Message::Undo,
            Message::Redo,
            Message::SoloHovered,
            Message::Toggle(BYPASS_PARAMETER),
            Message::Toggle(DELTA_PARAMETER),
        ] {
            let _ = state.update(Message::Shortcut(Box::new(command)));
        }
        assert_eq!(serde_json::to_vec(&state.parameters.snapshot()).expect("sound"), before);
        assert_eq!(binding.events.borrow().len(), callback_count);
        if context == 2 {
            assert_eq!(state.numeric_draft.as_ref().expect("draft").text, "6 dB");
        }
    }
}

/// Pointer-driven actions still resolve a draft before applying the requested sound command.
#[test]
fn direct_sound_actions_commit_drafts_without_global_shortcut_suppression() {
    let (mut state, binding) = editor();
    let _ = state.update(Message::NumericDraft(GAIN_PARAMETER, "6 dB".into()));
    let _ = state.update(Message::Toggle(MONO_PARAMETER));
    assert!(state.numeric_draft.is_none());
    assert!(state.parameters.get(MONO_PARAMETER).expect("Mono") >= 0.5);
    assert!((state.plain(GAIN_PARAMETER) - 6.0).abs() < 0.000_001);
    assert_eq!(
        binding
            .events
            .borrow()
            .iter()
            .filter(|(_, value, _)| value.is_some())
            .count(),
        2
    );
}

#[test]
fn reentrant_removal_during_begin_rejects_parameter_publication() {
    let (mut state, binding) = editor();
    binding.close_on_begin.set(true);
    let before = state.parameters.snapshot();
    let _ = state.update(Message::NumericDraft(GAIN_PARAMETER, "6 dB".into()));
    let _ = state.update(Message::NumericCommit(GAIN_PARAMETER));
    assert_eq!(
        serde_json::to_vec(&state.parameters.snapshot()).expect("state"),
        serde_json::to_vec(&before).expect("state")
    );
    assert!(binding.events.borrow().iter().all(|(_, value, _)| value.is_none()));
    assert!(state.gesture.is_none());
}

#[test]
fn numeric_drafts_commit_once_and_link_at_commit_time() {
    let (mut state, binding) = editor();
    let mid = band_parameter_id(0, 0).expect("Mid");
    let side = band_parameter_id(0, 1).expect("Side");
    let _ = state.update(Message::Link(0, true));
    let _ = state.update(Message::NumericFocus(mid));
    let _ = state.update(Message::NumericDraft(mid, "-6 dB".into()));
    let _ = state.update(Message::Refresh);
    assert!(binding.events.borrow().is_empty());
    let _ = state.update(Message::NumericCommit(mid));
    assert!((state.plain(mid) + 6.0).abs() < 0.000_001);
    assert!((state.plain(side) + 6.0).abs() < 0.000_001);
    assert!(state.gesture.is_none());
    let count = binding.events.borrow().len();
    let _ = state.update(Message::NumericBlur(mid));
    assert_eq!(binding.events.borrow().len(), count);
    let events = binding.events.borrow();
    assert_eq!(events.first(), Some(&(mid, None, true)));
    assert_eq!(events.last(), Some(&(mid, None, false)));
    assert_eq!(events.iter().filter(|(_, value, _)| value.is_some()).count(), 2);
}

#[test]
fn invalid_cancelled_and_clean_drafts_never_overwrite_automation() {
    let (mut state, binding) = editor();
    for draft in ["NaN", "inf", "1e999 dB", "garbage", ""] {
        let _ = state.update(Message::NumericDraft(GAIN_PARAMETER, draft.into()));
        let _ = state.update(Message::NumericCommit(GAIN_PARAMETER));
        assert!(state.numeric_draft.as_ref().expect("invalid draft").invalid);
        let _ = state.update(Message::NumericBlur(GAIN_PARAMETER));
        assert!(state.numeric_draft.is_none());
    }
    let _ = state.update(Message::NumericDraft(GAIN_PARAMETER, "6 dB".into()));
    assert!(state.parameters.set(GAIN_PARAMETER, 0.25));
    let _ = state.update(Message::Refresh);
    assert_eq!(state.numeric_draft.as_ref().expect("draft").text, "6 dB");
    let _ = state.update(Message::NumericCancel(GAIN_PARAMETER));
    assert_eq!(state.parameters.get(GAIN_PARAMETER), Some(0.25));
    let _ = state.update(Message::NumericFocus(GAIN_PARAMETER));
    assert!(state.parameters.set(GAIN_PARAMETER, 0.75));
    let _ = state.update(Message::NumericBlur(GAIN_PARAMETER));
    assert_eq!(state.parameters.get(GAIN_PARAMETER), Some(0.75));
    assert!(binding.events.borrow().is_empty());
}

#[test]
fn numeric_special_values_and_interrupted_teardown_remain_local_until_commit() {
    let (mut state, binding) = editor();
    for (field, text, expected) in [
        (0, "-inf dB", 0.0),
        (2, "1.25", 0.625),
        (3, "25%", 0.125),
        (3, "100%", 0.5),
        (3, "200%", 1.0),
        (4, "C", 0.5),
        (4, "50L", 0.25),
    ] {
        let id = band_parameter_id(0, field).expect("band field");
        let spec = parameter_spec(id).expect("metadata");
        assert_eq!(parse_numeric(spec, text), Some(expected));
    }
    let _ = state.update(Message::NumericDraft(GAIN_PARAMETER, "6 dB".into()));
    drop(state);
    assert!(binding.events.borrow().is_empty());
}

#[test]
fn linked_gesture_captures_participants_until_release_even_when_link_changes() {
    let (mut state, binding) = editor();
    let mid = band_parameter_id(0, 0).expect("Mid");
    let side = band_parameter_id(0, 1).expect("Side");
    let _ = state.update(Message::Link(0, true));
    let _ = state.update(Message::Change(mid, 0.75));
    let _ = state.update(Message::Link(0, false));
    let _ = state.update(Message::Change(mid, 0.5));
    let _ = state.update(Message::EndGesture);
    let events = binding.events.borrow();
    let starts: Vec<_> = events
        .iter()
        .filter(|(_, value, begin)| value.is_none() && *begin)
        .map(|(id, _, _)| *id)
        .collect();
    let ends: Vec<_> = events
        .iter()
        .filter(|(_, value, begin)| value.is_none() && !*begin)
        .map(|(id, _, _)| *id)
        .collect();
    assert_eq!(starts, [mid, side]);
    assert_eq!(ends, [side, mid]);
    assert!((state.plain(mid) - state.plain(side)).abs() < 0.000_001);
    assert!((state.plain(band_parameter_id(0, 2).expect("Width")) - 1.0).abs() < f64::EPSILON);
}

#[test]
fn pointer_pause_never_splits_a_held_gesture_and_drop_closes_it() {
    let (mut state, binding) = editor();
    let _ = state.update(Message::Hold);
    let _ = state.update(Message::Change(GAIN_PARAMETER, 0.7));
    state.gesture.as_mut().expect("gesture").last_change = Instant::now()
        .checked_sub(GESTURE_IDLE * 2)
        .expect("recent test instant");
    let _ = state.update(Message::Refresh);
    assert!(state.gesture.is_some());
    drop(state);
    let events = binding.events.borrow();
    assert_eq!(events.iter().filter(|(_, value, _)| value.is_none()).count(), 2);
    assert_eq!(events.last(), Some(&(GAIN_PARAMETER, None, false)));
}

#[test]
fn keyboard_release_closes_real_edit_and_toggle_is_balanced() {
    let (mut state, binding) = editor();
    let key = iced::keyboard::Key::Named(iced::keyboard::key::Named::ArrowUp);
    let event = iced::Event::Keyboard(iced::keyboard::Event::KeyReleased {
        key: key.clone(),
        modified_key: key,
        physical_key: iced::keyboard::key::Physical::Code(iced::keyboard::key::Code::ArrowUp),
        location: iced::keyboard::Location::Standard,
        modifiers: iced::keyboard::Modifiers::default(),
    });
    let _ = state.update(Message::Change(GAIN_PARAMETER, 0.8));
    let _ = state.update(release_message(&event).expect("key release"));
    assert!(state.gesture.is_none());
    let _ = state.update(Message::Toggle(MONO_PARAMETER));
    assert_eq!(binding.events.borrow().last(), Some(&(MONO_PARAMETER, None, false)));
    assert!(state.parameters.get(MONO_PARAMETER).expect("Mono") >= 0.5);
}

#[test]
fn automated_unordered_crossover_presentation_uses_the_same_ordering_as_audio() {
    let (state, binding) = editor();
    for (id, frequency) in CROSSOVER_PARAMETERS.into_iter().zip([5000.0, 100.0, 200.0]) {
        let spec = parameter_spec(id).expect("crossover");
        assert!(state.parameters.set(id, spec.plain_to_normalized(frequency)));
    }
    let expected = image_control_dsp::order_crossovers([5000.0, 100.0, 200.0]).expect("ordered");
    for (visible, ordered) in state.crossovers().into_iter().zip(expected) {
        assert!((visible - f64::from(ordered)).abs() < 0.001);
    }
    assert!(
        state
            .crossovers()
            .windows(2)
            .all(|pair| pair.first().expect("lower") < pair.get(1).expect("upper"))
    );
    assert!(binding.events.borrow().is_empty());
}

#[test]
fn marker_cannot_cross_its_neighbors_or_change_another_parameter() {
    let (mut state, binding) = editor();
    let id = CROSSOVER_PARAMETERS[1];
    let _ = state.update(Message::Change(id, 1.0));
    assert!((state.plain(id) - 5000.0 / f64::from(1.259_921_1_f32)).abs() < 0.001);
    assert!((state.plain(CROSSOVER_PARAMETERS[0]) - 100.0).abs() < 0.001);
    assert!((state.plain(CROSSOVER_PARAMETERS[2]) - 5000.0).abs() < 0.001);
    let _ = state.update(Message::EndGesture);
    assert!(binding.events.borrow().iter().all(|(changed, _, _)| *changed == id));
}

#[test]
fn divider_changes_layout_only_and_release_stops_dragging() {
    let (mut state, binding) = editor();
    let _ = state.update(Message::DividerStart(Point::new(500.0, 300.0)));
    let _ = state.update(Message::Pointer(Point::new(500.0, 280.0)));
    assert!((state.band_height - 324.0).abs() < f32::EPSILON);
    let _ = state.update(Message::EndGesture);
    let _ = state.update(Message::Pointer(Point::new(500.0, 200.0)));
    assert!((state.band_height - 324.0).abs() < f32::EPSILON);
    assert!(binding.events.borrow().is_empty());
}

#[test]
fn invalid_changes_and_automated_readback_do_not_create_host_gestures() {
    let (mut state, binding) = editor();
    for value in [f64::NAN, f64::INFINITY, -0.1, 1.1] {
        let _ = state.update(Message::Change(GAIN_PARAMETER, value));
    }
    assert!(state.parameters.set(GAIN_PARAMETER, 0.1));
    let _ = state.update(Message::Refresh);
    assert!(binding.events.borrow().is_empty());
    assert!((state.plain(GAIN_PARAMETER) + 20.4).abs() < 0.000_001);
}

/// Event-only renderer keeps real production widget tests available in Release as well as Debug.
/// It is never selected by the editor, and does not rasterize or generate any image.
struct EventRenderer;

impl iced_widget::graphics::geometry::Renderer for EventRenderer {
    type Geometry = <iced::Renderer as iced_widget::graphics::geometry::Renderer>::Geometry;
    type Frame = <iced::Renderer as iced_widget::graphics::geometry::Renderer>::Frame;

    fn new_frame(&self, bounds: iced::Rectangle) -> Self::Frame {
        iced_widget::renderer::wgpu::geometry::Frame::new(bounds)
    }

    fn draw_geometry(&mut self, _: Self::Geometry) {}
}

impl iced::advanced::Renderer for EventRenderer {
    fn start_layer(&mut self, _: iced::Rectangle) {}
    fn end_layer(&mut self) {}
    fn start_transformation(&mut self, _: iced::Transformation) {}
    fn end_transformation(&mut self) {}
    fn fill_quad(&mut self, _: iced::advanced::renderer::Quad, _: impl Into<iced::Background>) {}
    fn reset(&mut self, _: iced::Rectangle) {}
    fn allocate_image(
        &mut self,
        _: &iced::core::image::Handle,
        callback: impl FnOnce(Result<iced::core::image::Allocation, iced::core::image::Error>) + Send + 'static,
    ) {
        callback(Err(iced::core::image::Error::Unsupported));
    }
}

/// Routes actual events through retained production chrome, then the same raw-event subscription mapping.
fn drive_chrome(
    state: &mut EditorState,
    symbol: Symbol,
    message: Message,
    bounds: Rectangle,
    events: &[(iced::Event, iced::mouse::Cursor)],
) {
    let mut element: iced::core::Element<'_, Message, Theme, EventRenderer> =
        Chrome::new(symbol, bounds.size(), Some(message), false).into();
    let mut tree = iced::advanced::widget::Tree::new(&element);
    let node = iced::advanced::layout::Node::new(bounds.size()).move_to(bounds.position());
    for (event, cursor) in events {
        let mut messages = Vec::new();
        element.as_widget_mut().update(
            &mut tree,
            event,
            iced::advanced::Layout::new(&node),
            *cursor,
            &EventRenderer,
            &mut iced::advanced::clipboard::Null,
            &mut iced::advanced::Shell::new(&mut messages),
            &iced::Rectangle::with_size(Size::new(4096.0, 4096.0)),
        );
        for message in messages {
            let _ = state.update(message);
        }
        if let Some(message) = release_message(event) {
            let _ = state.update(message);
        }
    }
}

#[test]
fn real_resize_grip_captures_motion_outside_bounds_and_host_resize_notifications() {
    let (mut state, binding) = editor();
    let bounds = layout::rect(982.0, 746.0, 18.0, 18.0);
    let press = iced::Event::Mouse(iced::mouse::Event::ButtonPressed(iced::mouse::Button::Left));
    let release = iced::Event::Mouse(iced::mouse::Event::ButtonReleased(iced::mouse::Button::Left));
    drive_chrome(
        &mut state,
        Symbol::Resize,
        Message::Ignore,
        bounds,
        &[
            (press, iced::mouse::Cursor::Available(Point::new(992.0, 754.0))),
            (
                iced::Event::Mouse(iced::mouse::Event::CursorMoved {
                    position: Point::new(1092.0, 834.0),
                }),
                iced::mouse::Cursor::Unavailable,
            ),
        ],
    );
    assert_eq!(*binding.resizes.borrow(), [Size::new(1100.0, 844.0)]);
    let _ = state.update(Message::Resize);
    let _ = state.update(Message::Pointer(Point::new(1192.0, 934.0)));
    assert_eq!(binding.resizes.borrow().last(), Some(&Size::new(1200.0, 944.0)));
    let _ = state.update(release_message(&release).expect("raw release"));
    let _ = state.update(Message::Pointer(Point::new(1292.0, 1034.0)));
    assert_eq!(binding.resizes.borrow().len(), 2);
    assert!(binding.events.borrow().is_empty());
}

#[test]
fn real_disclosure_pointer_click_compacts_without_changing_sound_or_window() {
    let (mut state, binding) = editor();
    let before = state.parameters.snapshot();
    let bounds = layout::rect(476.0, 446.0, 48.0, 12.0);
    let cursor = iced::mouse::Cursor::Available(Point::new(500.0, 452.0));
    drive_chrome(
        &mut state,
        Symbol::Disclosure(false),
        Message::CompactBands,
        bounds,
        &[
            (
                iced::Event::Mouse(iced::mouse::Event::ButtonPressed(iced::mouse::Button::Left)),
                cursor,
            ),
            (
                iced::Event::Mouse(iced::mouse::Event::ButtonReleased(iced::mouse::Button::Left)),
                cursor,
            ),
        ],
    );
    assert!(state.compact_bands);
    assert_eq!(
        state.sections(state.size()).bands,
        layout::rect(8.0, 728.0, 984.0, 28.0)
    );
    assert_eq!(
        serde_json::to_vec(&state.parameters.snapshot()).expect("sound"),
        serde_json::to_vec(&before).expect("sound")
    );
    assert!(binding.resizes.borrow().is_empty());
    assert!(binding.events.borrow().is_empty());
}

#[test]
fn production_slider_routes_real_pointer_and_key_events_to_balanced_edits() {
    let (mut state, binding) = editor();
    let element: iced::core::Element<'_, Message, Theme, EventRenderer> = parameter_input(GAIN_PARAMETER, 0.5).into();
    let mut tree_widget = iced::advanced::widget::Tree::new(&element);
    let node = iced::advanced::layout::Node::new(iced::Size::new(200.0, 26.0));
    let bounds = iced::Rectangle::with_size(iced::Size::new(200.0, 26.0));
    let cursor = iced::mouse::Cursor::Available(iced::Point::new(150.0, 13.0));
    let mut element = element;
    let mut clipboard = iced::advanced::clipboard::Null;
    let key = iced::keyboard::Key::Named(iced::keyboard::key::Named::ArrowUp);
    let events = [
        iced::Event::Mouse(iced::mouse::Event::ButtonPressed(iced::mouse::Button::Left)),
        iced::Event::Mouse(iced::mouse::Event::ButtonReleased(iced::mouse::Button::Left)),
        iced::Event::Keyboard(iced::keyboard::Event::KeyPressed {
            key: key.clone(),
            modified_key: key.clone(),
            physical_key: iced::keyboard::key::Physical::Code(iced::keyboard::key::Code::ArrowUp),
            location: iced::keyboard::Location::Standard,
            modifiers: iced::keyboard::Modifiers::default(),
            text: None,
            repeat: false,
        }),
        iced::Event::Keyboard(iced::keyboard::Event::KeyReleased {
            key: key.clone(),
            modified_key: key,
            physical_key: iced::keyboard::key::Physical::Code(iced::keyboard::key::Code::ArrowUp),
            location: iced::keyboard::Location::Standard,
            modifiers: iced::keyboard::Modifiers::default(),
        }),
    ];
    for event in events {
        let mut messages = Vec::new();
        let mut shell = iced::advanced::Shell::new(&mut messages);
        element.as_widget_mut().update(
            &mut tree_widget,
            &event,
            iced::advanced::Layout::new(&node),
            cursor,
            &EventRenderer,
            &mut clipboard,
            &mut shell,
            &bounds,
        );
        for message in messages {
            let _ = state.update(message);
        }
        if let Some(message) = release_message(&event) {
            let _ = state.update(message);
        }
    }
    assert!(state.gesture.is_none());
    let callbacks = binding.events.borrow();
    assert_eq!(
        callbacks
            .iter()
            .filter(|(_, value, begin)| value.is_none() && *begin)
            .count(),
        2
    );
    assert_eq!(
        callbacks
            .iter()
            .filter(|(_, value, begin)| value.is_none() && !*begin)
            .count(),
        2
    );
    assert!(
        callbacks
            .iter()
            .any(|(_, value, _)| value.is_some_and(|value| (value - 0.75).abs() < 0.001))
    );
}

#[test]
fn real_preference_buttons_select_shared_state_without_sound_gestures() {
    let (mut state, binding) = editor();
    let before = serde_json::to_vec(&state.parameters.snapshot()).expect("sound");
    for message in [
        Message::Tap(AnalysisTap::Pre),
        Message::Settings,
        Message::Colour(ColourMode::Phase),
    ] {
        let content: iced::core::Element<'_, Message, Theme, EventRenderer> = Space::new().into();
        let mut element: iced::core::Element<'_, Message, Theme, EventRenderer> =
            control_input(content, 80.0, Some(message), false, theme::BLUE).into();
        let mut tree = iced::advanced::widget::Tree::new(&element);
        let node = element.as_widget_mut().layout(
            &mut tree,
            &EventRenderer,
            &iced::advanced::layout::Limits::new(iced::Size::ZERO, iced::Size::new(80.0, 24.0)),
        );
        let bounds = iced::Rectangle::with_size(iced::Size::new(80.0, 24.0));
        for event in [
            iced::Event::Mouse(iced::mouse::Event::ButtonPressed(iced::mouse::Button::Left)),
            iced::Event::Mouse(iced::mouse::Event::ButtonReleased(iced::mouse::Button::Left)),
        ] {
            let mut messages = Vec::new();
            element.as_widget_mut().update(
                &mut tree,
                &event,
                iced::advanced::Layout::new(&node),
                iced::mouse::Cursor::Available(iced::Point::new(40.0, 12.0)),
                &EventRenderer,
                &mut iced::advanced::clipboard::Null,
                &mut iced::advanced::Shell::new(&mut messages),
                &bounds,
            );
            for message in messages {
                let _ = state.update(message);
            }
        }
    }
    assert_eq!(state.shared.analysis_tap(), AnalysisTap::Pre);
    assert_eq!(state.shared.display_preferences().color_mode, ColourMode::Phase);
    assert!(state.settings_shown);
    assert!(binding.events.borrow().is_empty());
    assert_eq!(serde_json::to_vec(&state.parameters.snapshot()).expect("sound"), before);
    assert!(
        state
            .shared
            .set_display_preferences(image_control_dsp::DisplayPreferences::default())
    );
    state.shared.set_analysis_tap(AnalysisTap::Post);
    let _ = state.update(Message::Refresh);
    assert_eq!(state.shared.analysis_tap(), AnalysisTap::Post);
    assert_eq!(state.shared.display_preferences().color_mode, ColourMode::Frequency);
}

#[test]
fn maximized_plot_persists_without_changing_sound_geometry_or_analysis() {
    let (mut state, binding) = editor();
    let before = state.parameters.snapshot();
    let geometry = state.parameters.layout.snapshot();
    let source = state.shared.analysis_generation();
    let _ = state.update(Message::MaximizeSpectrogram);
    assert!(state.maximize_spectrogram);
    assert!(state.parameters.layout.preferences().maximize_spectrogram);
    assert_eq!(
        serde_json::to_vec(&state.parameters.snapshot()).expect("sound"),
        serde_json::to_vec(&before).expect("sound")
    );
    assert_eq!(state.parameters.layout.snapshot(), geometry);
    assert_eq!(state.shared.analysis_generation(), source);
    assert!(binding.events.borrow().is_empty());
    assert_eq!(binding.dirty_notifications.get(), 1);
    let _ = state.update(Message::Refresh);
    assert_eq!(binding.dirty_notifications.get(), 1);
    let parameters = Arc::clone(&state.parameters);
    let shared = Arc::clone(&state.shared);
    drop(state);
    let mut state = EditorState::for_host_test(parameters, shared, binding.clone());
    assert!(state.maximize_spectrogram);
    let _ = state.update(Message::ResetAppearance);
    assert!(state.parameters.layout.preferences().maximize_spectrogram);
    let _ = state.update(Message::MaximizeSpectrogram);
    assert!(!state.parameters.layout.preferences().maximize_spectrogram);
    assert_eq!(binding.dirty_notifications.get(), 2);
}

/// Geometry changes close every captured parameter exactly once and preserve the accepted numeric edit.
#[test]
fn maximize_resolves_numeric_drafts_and_balances_linked_and_band_gestures() {
    let (mut state, binding) = editor();
    let _ = state.update(Message::NumericDraft(GAIN_PARAMETER, "6 dB".into()));
    let _ = state.update(Message::MaximizeSpectrogram);
    assert!(state.numeric_draft.is_none());
    assert!(state.gesture.is_none());
    assert!((state.plain(GAIN_PARAMETER) - 6.0).abs() < 0.000_001);
    for event in [
        plot_overlay::PlotEvent::BeginGain(0, true),
        plot_overlay::PlotEvent::BeginBand(1),
    ] {
        let _ = state.update(Message::Link(0, true));
        let _ = state.update(Message::Plot(event));
        assert!(state.plot_gesture.is_some());
        let _ = state.update(Message::MaximizeSpectrogram);
        assert!(state.gesture.is_none());
        assert!(state.plot_gesture.is_none());
        let callbacks = binding.events.borrow();
        for id in [GAIN_PARAMETER, 1, 2, 4, 5] {
            assert_eq!(
                callbacks
                    .iter()
                    .filter(|(parameter, value, begin)| *parameter == id && value.is_none() && *begin)
                    .count(),
                callbacks
                    .iter()
                    .filter(|(parameter, value, begin)| *parameter == id && value.is_none() && !*begin)
                    .count()
            );
        }
    }
}

/// Reentrant host recall or removal during a gesture end cancels the pending layout operation.
#[test]
fn maximize_does_not_overwrite_reentrant_restore_or_continue_after_close() {
    for restore in [true, false] {
        let (mut state, binding) = editor();
        let _ = state.update(Message::Begin(GAIN_PARAMETER));
        let dirty_notifications = binding.dirty_notifications.get();
        if restore {
            let parameters = Arc::clone(&state.parameters);
            *binding.on_end.borrow_mut() = Some(Box::new(move || {
                assert!(parameters.restore(crate::params::Snapshot::default()));
            }));
        } else {
            let captured = Rc::clone(&binding);
            *binding.on_end.borrow_mut() = Some(Box::new(move || captured.closed.set(true)));
        }
        let _ = state.update(Message::MaximizeSpectrogram);
        assert!(!state.maximize_spectrogram);
        assert!(!state.parameters.layout.preferences().maximize_spectrogram);
        assert_eq!(binding.dirty_notifications.get(), dirty_notifications);
        assert!(state.gesture.is_none());
    }
}

/// Missing current-schema layout properties reset a live maximized editor to fixed defaults.
#[test]
fn maximize_follows_project_recall_and_missing_property_defaults() {
    let (mut state, _) = editor();
    let document = serde_json::json!({
        "version": crate::state::STATE_VERSION,
        "parameters": {},
        "layout": { "maximize_spectrogram": true }
    });
    let bytes = serde_json::to_vec(&document).expect("project");
    crate::state::restore_bytes(&bytes, &state.parameters, &state.shared, &state.parameters.layout).expect("recall");
    let _ = state.update(Message::Refresh);
    assert!(state.maximize_spectrogram);
    let bytes = serde_json::to_vec(&serde_json::json!({
        "version": crate::state::STATE_VERSION,
        "parameters": {},
        "layout": { "compact_bands": true }
    }))
    .expect("sparse project");
    crate::state::restore_bytes(&bytes, &state.parameters, &state.shared, &state.parameters.layout).expect("recall");
    let _ = state.update(Message::Refresh);
    assert!(!state.maximize_spectrogram);
    assert!(state.compact_bands);
}

/// A timer cadence cannot manufacture frames, while equal frame deltas produce the same measured rate.
#[test]
fn frame_rate_samples_encoded_composites_and_expires_when_rendering_stops() {
    let (mut state, _) = editor();
    let started = state.frame_rate_started;
    state.observe_frame_rate(started + FRAME_RATE_WINDOW * 2, 60);
    assert!((state.refresh_rate - 60.0).abs() < f32::EPSILON);
    state.observe_frame_rate(started + FRAME_RATE_WINDOW * 2 + Duration::from_millis(499), 90);
    assert!((state.refresh_rate - 60.0).abs() < f32::EPSILON);
    assert_eq!(state.measured_frames, 60);
    state.observe_frame_rate(started + FRAME_RATE_WINDOW * 3, 90);
    assert!((state.refresh_rate - 60.0).abs() < f32::EPSILON);
    state.observe_frame_rate(started + FRAME_RATE_WINDOW * 4, 90);
    assert!(state.refresh_rate.abs() < f32::EPSILON);
    state.observe_frame_rate(started + FRAME_RATE_WINDOW * 6, 0);
    assert!(state.refresh_rate.abs() < f32::EPSILON);
}

#[test]
fn revision_expiry_and_generation_changes_remove_real_meter_readings() {
    let now = Instant::now();
    let mut meter = CorrelationObservation::default();
    meter.observe(
        CorrelationSnapshot {
            readings: [1.0; 4],
            generation: 1,
            revision: 1,
        },
        now,
    );
    assert_eq!(meter.readings(now, Duration::ZERO), [None; 4]);
    meter.observe(
        CorrelationSnapshot {
            readings: [-0.5; 4],
            generation: 1,
            revision: 2,
        },
        now,
    );
    assert_eq!(meter.readings(now, Duration::ZERO), [Some(-0.5); 4]);
    assert_eq!(
        meter.readings(now + Duration::from_millis(250), Duration::ZERO),
        [None; 4]
    );
    assert_eq!(
        meter.readings(now + Duration::from_millis(300), Duration::from_secs(1)),
        [Some(-0.5); 4]
    );
    meter.observe(
        CorrelationSnapshot {
            readings: [1.0; 4],
            generation: 2,
            revision: 3,
        },
        now,
    );
    assert_eq!(meter.readings(now, Duration::from_secs(1)), [None; 4]);
}

#[test]
fn analysis_status_is_explicit_and_preference_validation_keeps_sound_unchanged() {
    assert_eq!(analysis_status_text(AnalysisStatus::Waiting), Some("Waiting for audio"));
    assert!(analysis_status_text(AnalysisStatus::UnsupportedRate).is_some());
    assert!(analysis_status_text(AnalysisStatus::Failed).is_some());
    assert!(analysis_status_text(AnalysisStatus::Running).is_none());
    let (mut state, binding) = editor();
    let display = state.shared.display_preferences();
    let _ = state.update(Message::Persistence(f32::NAN));
    let _ = state.update(Message::Contrast(2.0));
    assert_eq!(state.shared.display_preferences(), display);
    assert!(binding.events.borrow().is_empty());
}

#[test]
fn value_readouts_follow_independent_parameter_domains() {
    for (id, normalized, expected) in [
        (GAIN_PARAMETER, 2.0 / 3.0, "0.0 dB"),
        (4, 0.0, "-inf dB"),
        (6, 0.5, "1.00"),
        (7, 0.0, "0%"),
        (8, 0.5, "C"),
        (8, 0.25, "50L"),
    ] {
        assert_eq!(
            format_value(parameter_spec(id).expect("parameter"), normalized),
            expected
        );
    }
}

#[test]
fn raw_pointer_and_focus_loss_events_close_gestures() {
    for event in [
        iced::Event::Mouse(iced::mouse::Event::ButtonReleased(iced::mouse::Button::Left)),
        iced::Event::Window(iced::window::Event::Unfocused),
    ] {
        assert!(matches!(release_message(&event), Some(Message::EndGesture)));
    }
    assert!(release_message(&iced::Event::Mouse(iced::mouse::Event::CursorLeft)).is_none());
}

#[test]
fn marker_messages_use_registry_conversions_not_screen_units() {
    let Message::Change(id, normalized) = plot_message(plot_overlay::PlotEvent::Change(1, 2000.0)) else {
        panic!("change message")
    };
    assert_eq!(id, CROSSOVER_PARAMETERS[1]);
    let spec = parameter_spec(id).expect("crossover");
    assert_eq!(normalized.to_bits(), spec.plain_to_normalized(2000.0).to_bits());
    // The native float-skew round trip has float precision, not a double-precision frequency contract.
    assert!((spec.normalized_to_plain(normalized) - 2000.0).abs() <= 2000.0 * f64::from(f32::EPSILON) * 8.0);
}
