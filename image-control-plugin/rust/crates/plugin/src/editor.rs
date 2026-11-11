//! Retained Iced editor ownership, balanced host gestures, and uncommitted numeric drafts.

use std::rc::Rc;
use std::sync::Arc;
use std::time::{Duration, Instant};

use iced::keyboard::{Key, key::Named};
use iced::{Element, Point, Size, Subscription, Task, Theme};
use iced_baseview as iced;
use image_control_dsp::{AnalysisTap, ColourMode, SharedState};

#[path = "editor_chrome.rs"]
mod chrome;
#[path = "editor_commands.rs"]
mod commands;
#[path = "editor_controls.rs"]
mod controls;
#[path = "editor_render_telemetry.rs"]
pub mod editor_render_telemetry;
#[path = "layout.rs"]
mod layout;
#[path = "numeric_input.rs"]
mod numeric_input;
#[path = "plot_overlay.rs"]
mod plot_overlay;
#[path = "preset_editor.rs"]
mod presets;
#[path = "editor_refresh.rs"]
mod refresh;
#[cfg(test)]
#[path = "editor_tests.rs"]
mod tests;
#[path = "theme.rs"]
mod theme;
#[path = "editor_view.rs"]
mod view;

use crate::params::{
    BYPASS_PARAMETER, CROSSOVER_PARAMETERS, DELTA_PARAMETER, ParameterKind, ParameterSpec, Parameters,
    band_parameter_id, parameter_spec,
};
use crate::spectrogram;
#[cfg(test)]
use controls::CorrelationObservation;
use controls::{format_frequency, format_value};

/// Initial window size, shared with the native bridge.
pub const EDITOR_SIZE: (f32, f32) = layout::DEFAULT_SIZE;
/// Minimum dimensions shared with the host's resize constraints.
pub const MIN_EDITOR_SIZE: (f32, f32) = layout::MIN_SIZE;
/// Wheel edits have no release event, so their host gesture closes after a bounded idle period.
const GESTURE_IDLE: Duration = Duration::from_millis(200);
/// Encoded spectrum composites are sampled over a stable window, independently of GUI timer frequency.
const FRAME_RATE_WINDOW: Duration = Duration::from_millis(500);

pub use crate::params::EditorBinding;

/// Messages carry stable parameter ids instead of borrowed widget or host objects.
#[derive(Clone, Debug)]
pub enum Message {
    Presets,
    PresetName(String),
    PresetPath(String),
    PresetAction(presets::Action),
    PresetLoad(usize),
    ConfirmPreset,
    CancelPreset,
    Plot(plot_overlay::PlotEvent),
    SoloHovered,
    Shortcut(Box<Self>),
    Undo,
    Redo,
    Compare(usize),
    CopyComparison,
    DividerNudge(f32),
    MelScale(bool),
    SolidRendering(bool),
    PanMidpoint(f32),
    PanEdge(f32),
    SmoothCorrelation,
    ShowFps,
    HideSpectrogram,
    MaximizeSpectrogram,
    StopAnalysis,
    Shortcuts,
    NumericFocus(u32),
    NumericDraft(u32, String),
    NumericCommit(u32),
    NumericBlur(u32),
    NumericCancel(u32),
    FocusNext(bool),
    Change(u32, f64),
    Begin(u32),
    EndGesture,
    Toggle(u32),
    Link(usize, bool),
    Toolbar,
    Correlation,
    Settings,
    Tap(AnalysisTap),
    Persistence(f32),
    Contrast(f32),
    Colour(ColourMode),
    Refresh,
    Resize,
    Hold,
    DividerStart(Point),
    ResizeStart(Point),
    Pointer(Point),
    CompactBands,
    CloseSettings,
    ResetAppearance,
    Ignore,
}

/// Uncommitted text is GUI-owned and never published by refreshes or incoming automation.
struct NumericDraft {
    generation: u64,
    id: u32,
    text: String,
    baseline: String,
    invalid: bool,
    dirty: bool,
}

/// Captured drag origin is independent of host-accepted resize updates during raw pointer motion.
struct GeometryDrag {
    pointer: Point,
    size: Size,
}

/// Captured linked values preserve source-trim offsets and balanced automation when Link changes mid-edit.
struct Gesture {
    generation: u64,
    id: u32,
    linked: Option<(u32, f64, f64)>,
    last_change: Instant,
}

/// Multi-parameter plot gestures retain exact participants independently of the ordinary single-slider gesture.
struct PlotGesture {
    generation: u64,
    ids: Vec<u32>,
    gains: Option<(usize, bool, f64)>,
}

/// Native window construction retains immutable inputs, with all interactive state created at boot.
struct EditorProgram {
    parameters: Arc<Parameters>,
    shared: Arc<SharedState>,
    binding: Rc<dyn EditorBinding>,
    window_size: iced::shell::SharedWindowSize,
    notifier: Option<iced::PollSubNotifier>,
    analysis: Rc<std::cell::RefCell<Option<image_control_dsp::AnalysisShutdown>>>,
}

/// Window-owned state keeps parameter atomics authoritative and never holds mutable audio-processing state.
pub struct EditorState {
    restore_generation: u64,
    parameters: Arc<Parameters>,
    shared: Arc<SharedState>,
    settings_shown: bool,
    binding: Rc<dyn EditorBinding>,
    spectrogram: Result<spectrogram::Spectrogram, std::io::Error>,
    window_size: Option<iced::shell::SharedWindowSize>,
    notifier: Option<iced::PollSubNotifier>,
    gesture: Option<Gesture>,
    numeric_draft: Option<NumericDraft>,
    presets: presets::PresetEditor,
    linked: [bool; 4],
    toolbar_shown: bool,
    correlation_shown: bool,
    input_held: bool,
    input_cancelled: bool,
    band_height: f32,
    divider_drag: Option<GeometryDrag>,
    resize_drag: Option<GeometryDrag>,
    compact_bands: bool,
    spectrogram_hidden: bool,
    maximize_spectrogram: bool,
    analysis_stopped: bool,
    shortcuts_shown: bool,
    plot_band: Option<usize>,
    plot_gesture: Option<PlotGesture>,
    refresh_rate: f32,
    frame_rate_started: Instant,
    measured_frames: u64,
    editor_render_telemetry: Option<editor_render_telemetry::Recorder>,
}

impl iced_program::Program for EditorProgram {
    type State = EditorState;
    type Message = Message;
    type Theme = Theme;
    type Renderer = iced::Renderer;
    type Executor = crate::iced_executor::Executor;

    fn name() -> &'static str {
        "image_control"
    }

    fn settings(&self) -> iced::Settings {
        iced::Settings {
            fonts: vec![
                theme::ROBOTO.into(),
                theme::ROBOTO_SEMIBOLD.into(),
                theme::ROBOTO_BOLD.into(),
                theme::ROBOTO_MONO.into(),
            ],
            default_font: theme::TEXT,
            default_text_size: theme::LABEL_SIZE.into(),
            vsync: false,
            ..iced::Settings::default()
        }
    }

    fn window(&self) -> Option<iced::window::Settings> {
        Some(iced::window::Settings {
            size: Size::new(EDITOR_SIZE.0, EDITOR_SIZE.1),
            min_size: Some(Size::new(MIN_EDITOR_SIZE.0, MIN_EDITOR_SIZE.1)),
            ..iced::window::Settings::default()
        })
    }

    fn boot(&self) -> (EditorState, Task<Message>) {
        if let Some(notifier) = &self.notifier {
            notifier.notify();
        }
        let preferences = self.parameters.layout.preferences();
        self.shared.set_correlation_enabled(preferences.correlation_shown);
        let editor_render_telemetry = editor_render_telemetry::Recorder::from_environment();
        let mut spectrogram = spectrogram::Spectrogram::new(Arc::clone(&self.shared));
        if let (Some(recorder), Ok(spectrogram)) = (&editor_render_telemetry, &mut spectrogram) {
            spectrogram.set_frame_telemetry(recorder.frame_samples());
        }
        if let Ok(spectrogram) = &spectrogram {
            *self.analysis.borrow_mut() = Some(spectrogram.shutdown_handle());
        }
        self.shared
            .set_analysis_enabled(!preferences.analysis_stopped && !preferences.spectrogram_hidden);
        (
            EditorState {
                restore_generation: self.parameters.restore_generation(),
                parameters: Arc::clone(&self.parameters),
                shared: Arc::clone(&self.shared),
                settings_shown: false,
                binding: Rc::clone(&self.binding),
                spectrogram,
                window_size: Some(self.window_size.clone()),
                notifier: self.notifier.clone(),
                gesture: None,
                numeric_draft: None,
                presets: presets::PresetEditor::with_binding(Rc::clone(&self.binding)),
                linked: preferences.linked,
                toolbar_shown: preferences.toolbar_shown,
                correlation_shown: preferences.correlation_shown,
                input_held: false,
                input_cancelled: false,
                band_height: preferences.band_height,
                divider_drag: None,
                resize_drag: None,
                compact_bands: preferences.compact_bands,
                spectrogram_hidden: preferences.spectrogram_hidden,
                maximize_spectrogram: preferences.maximize_spectrogram,
                analysis_stopped: preferences.analysis_stopped,
                shortcuts_shown: false,
                plot_band: None,
                plot_gesture: None,
                refresh_rate: 0.0,
                frame_rate_started: Instant::now(),
                measured_frames: 0,
                editor_render_telemetry,
            },
            Task::none(),
        )
    }

    fn update(&self, state: &mut EditorState, message: Message) -> Task<Message> {
        state.update(message)
    }
    fn view<'a>(&self, state: &'a EditorState, _: iced::window::Id) -> Element<'a, Message> {
        state.view()
    }
    fn subscription(&self, state: &EditorState) -> Subscription<Message> {
        Subscription::batch([
            refresh::subscription(
                Arc::clone(&state.parameters),
                Arc::clone(&state.shared),
                state.presets.busy() || state.gesture.is_some(),
                state.notifier.clone(),
            ),
            EditorState::subscription(),
        ])
    }
    fn theme(&self, _: &EditorState, _: iced::window::Id) -> Option<Theme> {
        Some(Theme::Dark)
    }
    fn title(&self, _: &EditorState, _: iced::window::Id) -> String {
        "ImageControl".into()
    }
}

impl EditorState {
    /// Builds the production update owner without native resources for host-binding contract tests.
    #[cfg(test)]
    pub fn for_host_test(
        parameters: Arc<Parameters>,
        shared: Arc<SharedState>,
        binding: Rc<dyn EditorBinding>,
    ) -> Self {
        let preferences = parameters.layout.preferences();
        Self {
            restore_generation: parameters.restore_generation(),
            parameters,
            shared,
            settings_shown: false,
            binding,
            spectrogram: Err(std::io::Error::other("headless host interaction test")),
            window_size: None,
            notifier: None,
            gesture: None,
            numeric_draft: None,
            presets: presets::PresetEditor::default(),
            linked: preferences.linked,
            toolbar_shown: preferences.toolbar_shown,
            correlation_shown: preferences.correlation_shown,
            input_held: false,
            input_cancelled: false,
            band_height: preferences.band_height,
            divider_drag: None,
            resize_drag: None,
            compact_bands: preferences.compact_bands,
            spectrogram_hidden: preferences.spectrogram_hidden,
            maximize_spectrogram: preferences.maximize_spectrogram,
            analysis_stopped: preferences.analysis_stopped,
            shortcuts_shown: false,
            plot_band: None,
            plot_gesture: None,
            refresh_rate: 0.0,
            frame_rate_started: Instant::now(),
            measured_frames: 0,
            editor_render_telemetry: None,
        }
    }

    /// Applies only local user messages, leaving host automation as atomics read directly by each view.
    pub fn update(&mut self, message: Message) -> Task<Message> {
        if self.synchronize_restore()
            && matches!(
                message,
                Message::Change(..)
                    | Message::NumericDraft(..)
                    | Message::NumericCommit(..)
                    | Message::NumericBlur(..)
                    | Message::Pointer(..)
            )
        {
            return Task::none();
        }
        match message {
            Message::Presets
            | Message::PresetName(_)
            | Message::PresetPath(_)
            | Message::PresetAction(_)
            | Message::ConfirmPreset
            | Message::CancelPreset
            | Message::PresetLoad(_) => self.update_preset(message),
            Message::MelScale(_)
            | Message::SolidRendering(_)
            | Message::PanMidpoint(_)
            | Message::PanEdge(_)
            | Message::SmoothCorrelation
            | Message::ShowFps
            | Message::Persistence(_)
            | Message::Contrast(_)
            | Message::Colour(_)
            | Message::ResetAppearance => self.update_display(&message),
            Message::DividerNudge(_)
            | Message::HideSpectrogram
            | Message::MaximizeSpectrogram
            | Message::StopAnalysis
            | Message::Shortcuts
            | Message::CompactBands
            | Message::CloseSettings
            | Message::Toolbar
            | Message::Correlation
            | Message::Settings
            | Message::Tap(_) => self.update_layout(&message),
            Message::Plot(event) => self.update_plot(event),
            Message::Shortcut(command) => {
                if matches!(
                    *command,
                    Message::Undo | Message::Redo | Message::SoloHovered | Message::Toggle(_)
                ) && !self.sound_shortcuts_enabled()
                {
                    return Task::none();
                }
                return self.update(*command);
            }
            Message::SoloHovered => {
                if let Some(band) = self.plot_band {
                    self.toggle_parameter(band_parameter_id(band, 5).expect("band Solo"));
                }
            }
            Message::Undo => self.replay_history(true),
            Message::Redo => self.replay_history(false),
            Message::Compare(slot) => self.switch_comparison(slot),
            Message::CopyComparison => self.copy_comparison(),
            Message::FocusNext(backwards) => return self.focus_next(backwards),
            Message::NumericFocus(_)
            | Message::NumericDraft(..)
            | Message::NumericCommit(_)
            | Message::NumericBlur(_)
            | Message::NumericCancel(_) => self.update_numeric(message),
            Message::Begin(id) => self.begin_pointer_gesture(id),
            Message::Hold => self.input_held = true,
            Message::DividerStart(pointer) => self.start_divider(pointer),
            Message::ResizeStart(pointer) => self.start_resize(pointer),
            Message::Pointer(pointer) => self.move_pointer(pointer),
            Message::Change(id, value) => self.change_parameter(id, value),
            Message::EndGesture => {
                self.input_cancelled = false;
                self.divider_drag = None;
                self.resize_drag = None;
                self.input_held = false;
                self.end_gesture();
            }
            Message::Resize => {
                self.input_held = false;
                self.end_gesture();
            }
            Message::Toggle(id) => self.toggle_parameter(id),
            Message::Link(band, linked) => {
                if let Some(value) = self.linked.get_mut(band) {
                    *value = linked;
                }
            }
            Message::Refresh => self.refresh(),
            Message::Ignore => {}
        }
        if !self.synchronize_restore() {
            self.remember_layout();
        }
        self.synchronize_restore();
        Task::none()
    }

    /// Keeps global sound shortcuts out of modal dialogs and uncommitted text fields.
    const fn sound_shortcuts_enabled(&self) -> bool {
        !self.settings_shown && !self.presets.shown && self.numeric_draft.is_none()
    }

    /// Captures view choices before notifying hosts, without retaining editor state in the state codec.
    fn remember_layout(&self) {
        let preferences = crate::editor_preferences::LayoutPreferences {
            linked: self.linked,
            toolbar_shown: self.toolbar_shown,
            correlation_shown: self.correlation_shown,
            compact_bands: self.compact_bands,
            band_height: self.band_height.clamp(layout::MIN_BAND_HEIGHT, 4096.0),
            spectrogram_hidden: self.spectrogram_hidden,
            maximize_spectrogram: self.maximize_spectrogram,
            analysis_stopped: self.analysis_stopped,
        };
        if preferences != self.parameters.layout.preferences()
            && self.parameters.layout.publish_preferences(preferences)
        {
            self.binding.mark_dirty();
        }
    }

    /// Starts a captured pointer gesture only if resolving the previous draft did not trigger host recall.
    fn begin_pointer_gesture(&mut self, id: u32) {
        let generation = self.parameters.restore_generation();
        self.input_cancelled = false;
        self.finish_numeric();
        if generation == self.parameters.restore_generation() {
            self.input_held = true;
            self.begin_gesture(id);
        }
    }

    /// Rejects remaining movement after recall, including when the raw press arrives after its first slider update.
    fn change_parameter(&mut self, id: u32, value: f64) {
        if self.input_cancelled {
            return;
        }
        let generation = self.parameters.restore_generation();
        self.finish_numeric();
        if generation == self.parameters.restore_generation() {
            self.change(id, value);
        }
        self.input_cancelled |= generation != self.parameters.restore_generation();
    }

    /// Resolves the previous draft before native traversal and cancels it if a host callback recalls state.
    fn focus_next(&mut self, backwards: bool) -> Task<Message> {
        if self.settings_shown && !self.presets.shown {
            return Task::none();
        }
        if let Some(id) = self.numeric_draft.as_ref().map(|draft| draft.id) {
            self.commit_numeric(id, false);
        }
        self.synchronize_restore();
        if backwards {
            iced::widget::operation::focus_previous()
        } else {
            iced::widget::operation::focus_next()
        }
    }

    /// Keeps numeric draft messages together without changing their host gesture or native focus semantics.
    fn update_numeric(&mut self, message: Message) {
        match message {
            Message::NumericFocus(id) => self.focus_numeric(id),
            Message::NumericDraft(id, text) => self.edit_numeric(id, text),
            Message::NumericCommit(id) => self.commit_numeric(id, false),
            Message::NumericBlur(id) => self.commit_numeric(id, true),
            Message::NumericCancel(id) if self.numeric_draft.as_ref().is_some_and(|draft| draft.id == id) => {
                self.numeric_draft = None;
            }
            _ => {}
        }
    }

    /// Publishes a validated appearance change once, independently of sound automation.
    fn set_display_preferences(&self, display: image_control_dsp::DisplayPreferences) {
        if self.shared.display_preferences() != display && self.shared.set_display_preferences(display) {
            self.binding.mark_dirty();
        }
    }

    /// Discards pre-recall GUI work and closes its exact gesture participants without replaying any values.
    fn synchronize_restore(&mut self) -> bool {
        let generation = self.parameters.restore_generation();
        if self.restore_generation == generation {
            return false;
        }
        self.restore_generation = generation;
        self.presets.invalidate();
        let preferences = self.parameters.layout.preferences();
        self.linked = preferences.linked;
        self.toolbar_shown = preferences.toolbar_shown;
        self.correlation_shown = preferences.correlation_shown;
        self.compact_bands = preferences.compact_bands;
        self.band_height = preferences.band_height;
        self.spectrogram_hidden = preferences.spectrogram_hidden;
        self.maximize_spectrogram = preferences.maximize_spectrogram;
        self.analysis_stopped = preferences.analysis_stopped;
        self.shared.set_correlation_enabled(self.correlation_shown);
        self.shared
            .set_analysis_enabled(!self.analysis_stopped && !self.spectrogram_hidden);
        self.numeric_draft = None;
        self.input_cancelled |= self.input_held || self.gesture.is_some();
        self.input_held = false;
        self.divider_drag = None;
        self.resize_drag = None;
        self.end_gesture();
        true
    }

    /// Polls observational state without fabricating parameter gestures. Recalls discard unfinished numeric drafts.
    fn refresh(&mut self) {
        let benchmark_started = self.editor_render_telemetry.as_ref().map(|_| Instant::now());
        if self.gesture.is_none() && self.plot_gesture.is_none() {
            self.poll_presets();
        }
        let now = Instant::now();
        let rendered = self
            .spectrogram
            .as_ref()
            .map_or(0, spectrogram::Spectrogram::rendered_frame_count);
        self.observe_frame_rate(now, rendered);
        if !self.input_held
            && self
                .gesture
                .as_ref()
                .is_some_and(|gesture| gesture.last_change.elapsed() > GESTURE_IDLE)
        {
            self.end_gesture();
        }
        if let Some(notifier) = &self.notifier {
            notifier.notify();
        }
        let benchmark_size = self.editor_render_telemetry.as_ref().map(|_| self.size());
        if let (Some(recorder), Some(started), Some(size)) =
            (&mut self.editor_render_telemetry, benchmark_started, benchmark_size)
        {
            recorder.observe(started, rendered, size);
        }
    }

    /// Samples completed spectrum command encoding, never timer callbacks or physical display presentation.
    fn observe_frame_rate(&mut self, now: Instant, rendered: u64) {
        let window = now.saturating_duration_since(self.frame_rate_started);
        if window >= FRAME_RATE_WINDOW {
            self.refresh_rate = rendered.saturating_sub(self.measured_frames) as f32 / window.as_secs_f32();
            self.measured_frames = rendered;
            self.frame_rate_started = now;
        }
    }

    /// Subscribes to the bridge's timer and raw releases, including events already handled by a child widget.
    pub fn subscription() -> Subscription<Message> {
        Subscription::batch([
            iced::window_resized().map(|()| Message::Resize),
            iced::event::listen_raw(|event, status, _| {
                if status == iced::event::Status::Ignored
                    && let iced::Event::Keyboard(iced::keyboard::Event::KeyPressed {
                        key: iced::keyboard::Key::Named(iced::keyboard::key::Named::Tab),
                        modifiers,
                        ..
                    }) = &event
                {
                    Some(Message::FocusNext(modifiers.shift()))
                } else {
                    shortcut_message(&event)
                        .map(|message| Message::Shortcut(Box::new(message)))
                        .filter(|_| status == iced::event::Status::Ignored)
                        .or_else(|| release_message(&event))
                }
            }),
        ])
    }

    /// Captures section geometry for a view-only drag after resolving any numeric entry.
    fn start_divider(&mut self, pointer: Point) {
        self.finish_numeric();
        self.end_gesture();
        let size = self.size();
        self.band_height = self.sections(size).band;
        self.divider_drag = Some(GeometryDrag {
            pointer,
            size: Size::new(size.width, self.band_height),
        });
    }

    /// Captures the accepted geometry before requesting a host-owned resize drag.
    fn start_resize(&mut self, pointer: Point) {
        self.finish_numeric();
        self.end_gesture();
        self.resize_drag = Some(GeometryDrag {
            pointer,
            size: self.size(),
        });
    }

    /// Continues captured drags outside their widget bounds. Host acceptance remains authoritative.
    fn move_pointer(&mut self, pointer: Point) {
        if let Some(drag) = &self.divider_drag {
            self.band_height = (drag.size.height + drag.pointer.y - pointer.y).max(layout::MIN_BAND_HEIGHT);
        }
        if let Some(drag) = &self.resize_drag {
            let width = (drag.size.width + pointer.x - drag.pointer.x)
                .round()
                .clamp(layout::MIN_SIZE.0, layout::MAX_SIZE.0);
            let height = (drag.size.height + pointer.y - drag.pointer.y)
                .round()
                .clamp(layout::MIN_SIZE.1, layout::MAX_SIZE.1);
            self.binding.request_resize(width as u32, height as u32);
        }
    }

    /// Uses host-accepted logical dimensions, not a locally optimistic resize mirror.
    fn size(&self) -> Size {
        self.window_size
            .as_ref()
            .map_or(Size::new(EDITOR_SIZE.0, EDITOR_SIZE.1), |window| {
                let logical = window.get().logical;
                Size::new(logical.width as f32, logical.height as f32)
            })
    }

    /// Resolves view-only section choices against the current host-approved logical size.
    fn sections(&self, size: Size) -> layout::Sections {
        layout::Sections::with_visibility(
            size,
            self.toolbar_shown,
            self.correlation_shown,
            self.band_height,
            self.compact_bands,
            self.spectrogram_hidden,
        )
    }

    /// Toggles a switch using one host gesture after any pending numeric entry is resolved.
    fn toggle_parameter(&mut self, id: u32) {
        let generation = self.parameters.restore_generation();
        self.finish_numeric();
        self.end_gesture();
        if generation != self.parameters.restore_generation() {
            return;
        }
        if let Some(value) = self.parameters.get(id) {
            self.parameters.with_editing(|editing, _| editing.begin());
            self.binding.begin_edit(id);
            self.write(id, f64::from(value < 0.5), generation);
            self.binding.end_edit(id);
            self.parameters.with_editing(crate::editing::EditingState::end);
        }
    }

    /// Stores native typing without touching authoritative parameters until a commit.
    fn edit_numeric(&mut self, id: u32, text: String) {
        self.focus_numeric(id);
        if let Some(draft) = &mut self.numeric_draft {
            draft.dirty = text != draft.baseline;
            draft.text = text;
            draft.invalid = false;
        }
    }

    /// Starts a local draft without claiming a sound gesture or replacing unfinished typing on refresh.
    fn focus_numeric(&mut self, id: u32) {
        if self.settings_shown || self.presets.shown {
            return;
        }
        if self.numeric_draft.as_ref().is_some_and(|draft| draft.id == id) || parameter_spec(id).is_none() {
            return;
        }
        let generation = self.parameters.restore_generation();
        if let Some(previous) = self.numeric_draft.as_ref().map(|draft| draft.id) {
            self.commit_numeric(previous, true);
        }
        self.end_gesture();
        if generation != self.parameters.restore_generation() || !self.binding.is_open() {
            return;
        }
        let spec = parameter_spec(id).expect("validated numeric parameter");
        let value = self.parameters.get(id).unwrap_or(spec.default_normalized);
        self.numeric_draft = Some(NumericDraft {
            generation: self.parameters.restore_generation(),
            id,
            text: format_value(spec, value),
            baseline: format_value(spec, value),
            invalid: false,
            dirty: false,
        });
    }

    /// Ends numeric entry before another control hides it or changes the same parameter.
    fn finish_numeric(&mut self) {
        if let Some(id) = self.numeric_draft.as_ref().map(|draft| draft.id) {
            self.commit_numeric(id, true);
        }
    }

    /// Publishes a valid draft as one balanced host edit. Invalid blur cancels, while Enter keeps the error visible.
    fn commit_numeric(&mut self, id: u32, blur: bool) {
        let Some(mut draft) = self.numeric_draft.take() else {
            return;
        };
        if draft.generation != self.parameters.restore_generation() {
            return;
        }
        if draft.id != id {
            self.numeric_draft = Some(draft);
            return;
        }
        if !draft.dirty {
            if !blur {
                self.numeric_draft = Some(draft);
            }
            return;
        }
        let spec = parameter_spec(id).expect("numeric parameter metadata");
        let value = parse_numeric(spec, &draft.text);
        if let Some(value) = value {
            self.change(id, value);
            self.end_gesture();
            if !blur && draft.generation == self.parameters.restore_generation() && self.binding.is_open() {
                draft.text = format_value(spec, self.parameters.get(id).unwrap_or(spec.default_normalized));
                draft.baseline.clone_from(&draft.text);
                draft.dirty = false;
                draft.invalid = false;
                self.numeric_draft = Some(draft);
            }
        } else if !blur {
            draft.invalid = true;
            self.numeric_draft = Some(draft);
        }
    }

    /// Captures linked source trims once, so incoming automation never recursively drives Link.
    fn begin_gesture(&mut self, id: u32) {
        let generation = self.parameters.restore_generation();
        if self
            .gesture
            .as_ref()
            .is_some_and(|gesture| gesture.id == id && gesture.generation == generation)
        {
            return;
        }
        self.end_gesture();
        if parameter_spec(id).is_none() || !self.binding.is_open() || generation != self.parameters.restore_generation()
        {
            return;
        }
        let linked = (0..4).find_map(|band| {
            if !self.linked.get(band).copied().unwrap_or(false) || band_parameter_id(band, 0) != Some(id) {
                return None;
            }
            let side = band_parameter_id(band, 1)?;
            Some((side, self.plain(id), self.plain(side)))
        });
        self.binding.begin_edit(id);
        if !self.binding.is_open() || generation != self.parameters.restore_generation() {
            self.binding.end_edit(id);
            return;
        }
        if let Some((side, _, _)) = linked {
            self.binding.begin_edit(side);
            if !self.binding.is_open() || generation != self.parameters.restore_generation() {
                self.binding.end_edit(side);
                self.binding.end_edit(id);
                return;
            }
        }
        self.parameters.with_editing(|editing, _| editing.begin());
        self.gesture = Some(Gesture {
            generation,
            id,
            linked,
            last_change: Instant::now(),
        });
    }

    /// Writes finite host-normalized values and moves linked Side by the same plain dB amount as Mid.
    fn change(&mut self, id: u32, value: f64) {
        let generation = self.parameters.restore_generation();
        if !value.is_finite() || !(0.0..=1.0).contains(&value) || parameter_spec(id).is_none() {
            return;
        }
        let value = CROSSOVER_PARAMETERS
            .iter()
            .position(|candidate| *candidate == id)
            .map_or(value, |index| {
                let crossovers = self.crossovers();
                let ratio = f64::from(1.259_921_1_f32);
                let lower = index
                    .checked_sub(1)
                    .and_then(|previous| crossovers.get(previous))
                    .map_or(20.0, |frequency| frequency * ratio);
                let upper = crossovers
                    .get(index + 1)
                    .map_or(20_000.0, |frequency| frequency / ratio);
                let spec = parameter_spec(id).expect("validated crossover");
                spec.plain_to_normalized(spec.normalized_to_plain(value).clamp(lower, upper.max(lower)))
            });
        self.begin_gesture(id);
        let Some(gesture) = &mut self.gesture else {
            return;
        };
        if generation != gesture.generation || generation != self.parameters.restore_generation() {
            return;
        }
        gesture.last_change = Instant::now();
        let linked = gesture.linked;
        self.write(id, value, generation);
        if let Some((side, start_mid, start_side)) = linked
            && start_side > -60.0
            && let (Some(mid_spec), Some(side_spec)) = (parameter_spec(id), parameter_spec(side))
        {
            let side_db = start_side + mid_spec.normalized_to_plain(value) - start_mid;
            self.write(side, side_spec.plain_to_normalized(side_db), generation);
        }
    }

    /// Closes every captured participant even if its Link checkbox changed during the edit.
    fn end_gesture(&mut self) {
        if let Some(plot) = self.plot_gesture.take() {
            for id in plot.ids.iter().rev() {
                self.binding.end_edit(*id);
            }
            if !plot.ids.is_empty() {
                self.parameters.with_editing(crate::editing::EditingState::end);
            }
        }
        if let Some(gesture) = self.gesture.take() {
            if let Some((side, _, _)) = gesture.linked {
                self.binding.end_edit(side);
            }
            self.binding.end_edit(gesture.id);
            self.parameters.with_editing(crate::editing::EditingState::end);
        }
    }

    /// Lets the adapter admit automation transport before changing authoritative values, never a temporary UI mirror.
    fn write(&self, id: u32, value: f64, generation: u64) {
        if self.binding.is_open() && generation == self.parameters.restore_generation() {
            if let Some(before) = self.parameters.get(id) {
                self.parameters.with_editing(|editing, _| editing.touch(id, before));
            }
            self.binding.publish_value(&self.parameters, id, value);
        }
    }

    /// Reads a plain value for layout labels and linked dB offsets.
    fn plain(&self, id: u32) -> f64 {
        parameter_spec(id).map_or(0.0, |spec| {
            spec.normalized_to_plain(self.parameters.get(id).unwrap_or(spec.default_normalized))
        })
    }

    /// Reads all three visible markers from the same registry used by the host and sliders.
    fn crossovers(&self) -> [f64; 3] {
        let requested = CROSSOVER_PARAMETERS.map(|id| self.plain(id) as f32);
        image_control_dsp::order_crossovers(requested)
            .expect("finite registry crossover values")
            .map(f64::from)
    }
}

impl Drop for EditorState {
    /// Ends host edits before releasing the analysis worker and GPU identity on the GUI thread.
    fn drop(&mut self) {
        self.end_gesture();
        if let Some(recorder) = &self.editor_render_telemetry {
            // Missing or failed telemetry is reported by the dedicated host, never logged by a plugin callback.
            let _ = recorder.flush();
        }
    }
}

/// Shares the window's poll notifier so each bounded timer callback rearms the next analyzer refresh.
pub fn create_program_with_notifier(
    parameters: Arc<Parameters>,
    shared: Arc<SharedState>,
    binding: Rc<dyn EditorBinding>,
    window_size: iced::shell::SharedWindowSize,
    notifier: iced::PollSubNotifier,
    analysis: Rc<std::cell::RefCell<Option<image_control_dsp::AnalysisShutdown>>>,
) -> impl iced_program::Program<State = EditorState, Message = Message, Theme = Theme, Renderer = iced::Renderer> {
    EditorProgram {
        parameters,
        shared,
        binding,
        window_size,
        notifier: Some(notifier),
        analysis,
    }
}

/// Converts real marker gestures to registry-normalized events without a separate crossover slider row.
fn plot_message(event: plot_overlay::PlotEvent) -> Message {
    match event {
        plot_overlay::PlotEvent::Begin(index) => {
            Message::Begin(*CROSSOVER_PARAMETERS.get(index).expect("fixed crossover id"))
        }
        plot_overlay::PlotEvent::Change(index, frequency) => {
            let id = *CROSSOVER_PARAMETERS.get(index).expect("fixed crossover id");
            let spec = parameter_spec(id).expect("fixed crossover id");
            Message::Change(id, spec.plain_to_normalized(frequency))
        }
        plot_overlay::PlotEvent::End => Message::EndGesture,
        event => Message::Plot(event),
    }
}

/// Routes uncaptured plugin commands, leaving ordinary text-input shortcuts with their focused field.
fn shortcut_message(event: &iced::Event) -> Option<Message> {
    let iced::Event::Keyboard(iced::keyboard::Event::KeyPressed { key, modifiers, .. }) = event else {
        return None;
    };
    if matches!(key, Key::Named(Named::F1)) {
        return Some(Message::Shortcuts);
    }
    if !modifiers.command()
        && !modifiers.alt()
        && matches!(key, Key::Character(character) if character.eq_ignore_ascii_case("s"))
    {
        return Some(Message::SoloHovered);
    }
    if !modifiers.command() {
        return None;
    }
    let Key::Character(character) = key else {
        return None;
    };
    match character.to_lowercase().as_str() {
        "z" => Some(if modifiers.shift() {
            Message::Redo
        } else {
            Message::Undo
        }),
        "y" => Some(Message::Redo),
        "b" => Some(Message::Toggle(BYPASS_PARAMETER)),
        "d" => Some(Message::Toggle(DELTA_PARAMETER)),
        "," => Some(Message::Settings),
        _ => None,
    }
}

/// Closes pointer, keyboard, touch, and interrupted gestures even when a child captured their release.
const fn release_message(event: &iced::Event) -> Option<Message> {
    match event {
        iced::Event::Mouse(iced::mouse::Event::CursorMoved { position }) => Some(Message::Pointer(*position)),
        iced::Event::Keyboard(iced::keyboard::Event::KeyPressed {
            key: iced::keyboard::Key::Named(iced::keyboard::key::Named::Escape),
            ..
        }) => Some(Message::CloseSettings),
        iced::Event::Keyboard(iced::keyboard::Event::KeyPressed { .. })
        | iced::Event::Mouse(iced::mouse::Event::ButtonPressed(iced::mouse::Button::Left))
        | iced::Event::Touch(iced::touch::Event::FingerPressed { .. }) => Some(Message::Hold),
        iced::Event::Keyboard(iced::keyboard::Event::KeyReleased { .. })
        | iced::Event::Mouse(iced::mouse::Event::ButtonReleased(iced::mouse::Button::Left))
        | iced::Event::Touch(iced::touch::Event::FingerLifted { .. } | iced::touch::Event::FingerLost { .. })
        | iced::Event::Window(iced::window::Event::Unfocused) => Some(Message::EndGesture),
        _ => None,
    }
}

/// Accepts the displayed mute spelling locally without changing the host's finite parameter parser.
fn parse_numeric(spec: ParameterSpec, text: &str) -> Option<f64> {
    if spec.kind == ParameterKind::BandGain
        && matches!(
            text.trim().to_ascii_lowercase().as_str(),
            "-inf" | "-inf db" | "-∞" | "-∞ db"
        )
    {
        Some(0.0)
    } else {
        spec.parse(text)
    }
}
