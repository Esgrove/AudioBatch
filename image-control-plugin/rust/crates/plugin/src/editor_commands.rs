//! Local history, comparison, preset, display, and plot command routing for the retained editor.

use super::{EditorState, Message, PlotGesture, layout, plot_overlay, presets};
use crate::params::{CROSSOVER_PARAMETERS, band_parameter_id, parameter_spec};

impl EditorState {
    /// Keeps preset modal commands separate from sound and display updates.
    pub(super) fn update_preset(&mut self, message: Message) {
        match message {
            Message::Presets => {
                self.finish_numeric();
                self.end_gesture();
                self.presets.shown = !self.presets.shown;
                if self.presets.shown {
                    self.start_preset(presets::Action::Refresh);
                }
            }
            Message::PresetName(name) => self.presets.name = name,
            Message::PresetPath(path) => self.presets.path = path,
            Message::PresetAction(action) => {
                if matches!(
                    action,
                    presets::Action::Save | presets::Action::Rename | presets::Action::Delete
                ) {
                    self.presets.confirmation = Some(action);
                } else {
                    self.start_preset(action);
                }
            }
            Message::ConfirmPreset => {
                if let Some(action) = self.presets.confirmation.take() {
                    self.start_preset(action);
                }
            }
            Message::CancelPreset => {
                self.presets.confirmation = None;
                self.presets.shown = false;
            }
            Message::PresetLoad(index) => self.load_preset(index),
            _ => {}
        }
    }

    /// Applies appearance controls independently of host sound automation.
    pub(super) fn update_display(&self, message: &Message) {
        let mut display = self.shared.display_preferences();
        match *message {
            Message::MelScale(enabled) => display.mel_scale = enabled,
            Message::SolidRendering(enabled) => display.solid_rendering = enabled,
            Message::PanMidpoint(value) => {
                display.pan_midpoint_db = value.round().clamp(3.0, 24.0);
                display.pan_edge_db = display.pan_edge_db.max(display.pan_midpoint_db + 3.0);
            }
            Message::PanEdge(value) => {
                display.pan_edge_db = value.round().clamp(12.0, 60.0);
                display.pan_midpoint_db = display.pan_midpoint_db.min(display.pan_edge_db - 3.0);
            }
            Message::SmoothCorrelation => display.smooth_correlation = !display.smooth_correlation,
            Message::ShowFps => display.show_fps = !display.show_fps,
            Message::Persistence(seconds) => display.persistence_seconds = seconds,
            Message::Contrast(contrast) => display.contrast = contrast,
            Message::Colour(color_mode) => display.color_mode = color_mode,
            Message::ResetAppearance => display = image_control_dsp::DisplayPreferences::default(),
            _ => return,
        }
        self.set_display_preferences(display);
    }

    /// Handles view-only visibility and geometry commands without opening sound gestures.
    pub(super) fn update_layout(&mut self, message: &Message) {
        match *message {
            Message::DividerNudge(delta) => {
                self.band_height = (self.band_height + delta).clamp(layout::MIN_BAND_HEIGHT, 4096.0);
            }
            Message::HideSpectrogram => {
                self.spectrogram_hidden = !self.spectrogram_hidden;
                self.shared
                    .set_analysis_enabled(!self.analysis_stopped && !self.spectrogram_hidden);
            }
            Message::StopAnalysis => {
                self.analysis_stopped = !self.analysis_stopped;
                self.shared
                    .set_analysis_enabled(!self.analysis_stopped && !self.spectrogram_hidden);
            }
            Message::Shortcuts => {
                self.finish_numeric();
                self.end_gesture();
                self.settings_shown = true;
                self.shortcuts_shown = !self.shortcuts_shown;
            }
            Message::CompactBands => {
                self.finish_numeric();
                self.end_gesture();
                self.divider_drag = None;
                self.compact_bands = !self.compact_bands;
            }
            Message::CloseSettings => {
                if self.presets.shown {
                    self.presets.shown = false;
                    self.presets.confirmation = None;
                } else if self.shortcuts_shown {
                    self.shortcuts_shown = false;
                } else {
                    self.settings_shown = false;
                }
            }
            Message::Toolbar => {
                self.finish_numeric();
                self.end_gesture();
                self.toolbar_shown = !self.toolbar_shown;
            }
            Message::Correlation => {
                self.correlation_shown = !self.correlation_shown;
                self.shared.set_correlation_enabled(self.correlation_shown);
                self.correlation = super::CorrelationObservation::default();
                for history in &mut self.correlation_history {
                    history.clear();
                }
            }
            Message::Settings => {
                self.finish_numeric();
                self.settings_shown = !self.settings_shown;
            }
            Message::Tap(tap) if self.shared.analysis_tap() != tap => {
                self.shared.set_analysis_tap(tap);
                self.binding.mark_dirty();
            }
            _ => {}
        }
    }

    /// Derives modified status from the source baseline rather than serializing a stale derived flag.
    pub(super) fn preset_label(&self) -> String {
        let comparison = self
            .parameters
            .with_editing(|editing, snapshot| editing.comparison(snapshot));
        comparison
            .origins
            .get(comparison.active)
            .and_then(Option::as_ref)
            .map_or_else(
                || "Unsaved sound".into(),
                |origin| {
                    format!(
                        "{}{}",
                        origin.name,
                        if origin.effective.unwrap_or(origin.baseline)
                            == crate::editing::SoundSnapshot::capture(self.parameters.snapshot())
                        {
                            ""
                        } else {
                            " *"
                        }
                    )
                },
            )
    }

    /// Captures file-operation inputs on the UI thread and moves only detached values to the worker.
    fn start_preset(&mut self, action: presets::Action) {
        self.finish_numeric();
        self.end_gesture();
        let comparison = self
            .parameters
            .with_editing(|editing, snapshot| editing.comparison(snapshot));
        let identity = comparison
            .origins
            .get(comparison.active)
            .and_then(Option::as_ref)
            .map(|origin| origin.id.clone());
        if !matches!(
            action,
            presets::Action::Refresh
                | presets::Action::Load
                | presets::Action::SaveAs
                | presets::Action::Import
                | presets::Action::Reconnect
        ) && self
            .presets
            .bound
            .get(comparison.active)
            .and_then(Option::as_ref)
            .is_some_and(|preset| Some(&preset.id) != identity.as_ref())
        {
            self.presets.error =
                Some("The comparison's selected preset changed. Reconnect before modifying the library".into());
            return;
        }
        self.presets.start(
            action,
            self.parameters.restore_generation(),
            comparison.active,
            crate::editing::SoundSnapshot::capture(self.parameters.snapshot()),
            identity,
        );
    }

    /// Recall validates the selected complete sound without affecting audition switches or display choices.
    fn load_preset(&mut self, index: usize) {
        let Some(preset) = self.presets.entries.get(index).cloned() else {
            return;
        };
        let active = self
            .parameters
            .with_editing(|editing, snapshot| editing.comparison(snapshot))
            .active;
        *self.presets.bound.get_mut(active).expect("active slot") = Some(preset);
        self.start_preset(presets::Action::Load);
    }

    /// Applies an independently reread accepted library revision through normal sound transactions.
    fn recall_preset(&mut self, preset: crate::user_presets::UserPreset) {
        self.finish_numeric();
        self.end_gesture();
        let generation = self.parameters.restore_generation();
        self.parameters.with_editing(|editing, snapshot| {
            editing.begin();
            editing.select_preset(
                crate::editing::PresetOrigin {
                    id: preset.id.clone(),
                    name: preset.name.clone(),
                    baseline: preset.sound,
                    effective: None,
                },
                snapshot,
            );
        });
        self.recall_values(&preset.sound.parameters().collect::<Vec<_>>(), generation, true);
        if generation == self.parameters.restore_generation() {
            self.parameters.with_editing(|editing, snapshot| {
                editing.set_effective_origin(crate::editing::SoundSnapshot::capture(snapshot));
            });
        }
        self.parameters.with_editing(crate::editing::EditingState::end);
        if generation == self.parameters.restore_generation() && self.binding.is_open() {
            let active = self
                .parameters
                .with_editing(|editing, snapshot| editing.comparison(snapshot))
                .active;
            self.presets.name.clone_from(&preset.name);
            *self.presets.bound.get_mut(active).expect("active slot") = Some(preset);
            self.binding.mark_dirty();
        }
    }

    /// Applies file completions only to their captured restore generation, slot, and unchanged sound.
    pub(super) fn poll_presets(&mut self) {
        let Some(completion) = self.presets.poll() else {
            return;
        };
        let comparison = self
            .parameters
            .with_editing(|editing, snapshot| editing.comparison(snapshot));
        if completion.generation != self.parameters.restore_generation()
            || completion.epoch != self.presets.epoch
            || completion.slot != comparison.active
        {
            self.presets.error = Some("The project or comparison changed while the preset operation completed".into());
            return;
        }
        match completion.result {
            Err(error) => self.presets.error = Some(error),
            Ok(presets::Outcome::Listed(entries)) => {
                self.presets.entries = entries;
                self.presets.error = None;
            }
            Ok(presets::Outcome::Loaded(preset)) => {
                if completion.before == crate::editing::SoundSnapshot::capture(self.parameters.snapshot()) {
                    self.recall_preset(preset);
                } else {
                    self.presets.error = Some("Sound changed before preset loading completed. Select it again".into());
                }
            }
            Ok(presets::Outcome::Saved(preset)) => {
                if completion.before != crate::editing::SoundSnapshot::capture(self.parameters.snapshot()) {
                    self.presets.error = Some("The saved preset is in the library, but sound changed before it could be selected. Refresh to select it".into());
                    return;
                }
                self.parameters.with_editing(|editing, snapshot| {
                    editing.begin();
                    editing.select_preset(
                        crate::editing::PresetOrigin {
                            id: preset.id.clone(),
                            name: preset.name.clone(),
                            baseline: preset.sound,
                            effective: None,
                        },
                        snapshot,
                    );
                    editing.end(snapshot);
                });
                self.presets.name.clone_from(&preset.name);
                *self.presets.bound.get_mut(comparison.active).expect("active slot") = Some(preset);
                self.binding.mark_dirty();
                self.start_preset(presets::Action::Refresh);
            }
            Ok(presets::Outcome::Renamed(preset)) => {
                self.parameters.with_editing(|editing, snapshot| {
                    editing.begin();
                    editing.rename_origin(&preset.id, preset.name.clone(), snapshot);
                    editing.end(snapshot);
                });
                self.presets.name.clone_from(&preset.name);
                *self.presets.bound.get_mut(comparison.active).expect("active slot") = Some(preset);
                self.binding.mark_dirty();
                self.start_preset(presets::Action::Refresh);
            }
            Ok(presets::Outcome::Reconnected(preset)) => {
                *self.presets.bound.get_mut(comparison.active).expect("active slot") = Some(preset);
                self.presets.error = None;
            }
            Ok(presets::Outcome::Deleted) => {
                *self.presets.bound.get_mut(comparison.active).expect("active slot") = None;
                self.start_preset(presets::Action::Refresh);
            }
            Ok(presets::Outcome::Imported) => self.start_preset(presets::Action::Refresh),
            Ok(presets::Outcome::Exported) => self.presets.error = None,
        }
    }

    /// Adapts plot commands to authoritative writes, local history, and balanced host gesture boundaries.
    pub(super) fn update_plot(&mut self, event: plot_overlay::PlotEvent) {
        use plot_overlay::PlotEvent;
        match event {
            PlotEvent::Begin(index) => {
                self.finish_numeric();
                self.end_gesture();
                self.begin_pointer_gesture(*CROSSOVER_PARAMETERS.get(index).expect("fixed crossover index"));
            }
            PlotEvent::Change(index, frequency) => {
                let id = *CROSSOVER_PARAMETERS.get(index).expect("fixed crossover index");
                self.change_parameter(
                    id,
                    parameter_spec(id).expect("crossover").plain_to_normalized(frequency),
                );
            }
            PlotEvent::HoverBand(band) => self.plot_band = band,
            PlotEvent::Reset(index) => {
                let id = *CROSSOVER_PARAMETERS.get(index).expect("fixed crossover index");
                self.finish_numeric();
                self.end_gesture();
                self.change_parameter(id, parameter_spec(id).expect("crossover").default_normalized);
                self.end_gesture();
            }
            PlotEvent::BeginBand(band) => self.begin_band_gesture(band),
            PlotEvent::ChangeBand(frequencies) => {
                if let Some(gesture) = &self.plot_gesture {
                    for id in &gesture.ids {
                        let index = (*id - 1) as usize;
                        self.write(
                            *id,
                            parameter_spec(*id)
                                .expect("crossover")
                                .plain_to_normalized(*frequencies.get(index).expect("fixed crossover index")),
                            gesture.generation,
                        );
                    }
                }
            }
            PlotEvent::BeginGain(band, mid) => {
                self.finish_numeric();
                self.end_gesture();
                let id = band_parameter_id(band, usize::from(!mid)).expect("band gain");
                let baseline = self.plain(id);
                self.begin_pointer_gesture(id);
                self.plot_gesture = Some(PlotGesture {
                    generation: self.parameters.restore_generation(),
                    ids: Vec::new(),
                    gains: Some((band, mid, baseline)),
                });
            }
            PlotEvent::ChangeGain(band, mid, delta) => {
                if let Some(gesture) = &self.plot_gesture
                    && gesture.generation == self.parameters.restore_generation()
                    && let Some((captured_band, captured_mid, baseline)) = gesture.gains
                    && band == captured_band
                    && mid == captured_mid
                {
                    let id = band_parameter_id(band, usize::from(!mid)).expect("band gain");
                    self.change_parameter(
                        id,
                        parameter_spec(id).expect("gain").plain_to_normalized(baseline + delta),
                    );
                }
            }
            PlotEvent::End => {
                self.input_held = false;
                self.end_gesture();
            }
        }
    }

    /// Captures exactly the crossovers surrounding a dragged band before opening host edits.
    fn begin_band_gesture(&mut self, band: usize) {
        self.finish_numeric();
        self.end_gesture();
        let ids = (0..3)
            .filter(|index| *index == band || *index + 1 == band)
            .map(|index| *CROSSOVER_PARAMETERS.get(index).expect("fixed crossover index"))
            .collect::<Vec<_>>();
        let generation = self.parameters.restore_generation();
        self.parameters.with_editing(|editing, _| editing.begin());
        let mut opened = Vec::new();
        for id in ids {
            self.binding.begin_edit(id);
            opened.push(id);
            if !self.binding.is_open() || generation != self.parameters.restore_generation() {
                break;
            }
        }
        self.plot_gesture = Some(PlotGesture {
            generation,
            ids: opened,
            gains: None,
        });
        self.input_held = true;
    }

    /// Replays only touched values, without coupling linked controls or recording automation as local edits.
    pub(super) fn replay_history(&mut self, undo: bool) {
        self.finish_numeric();
        self.end_gesture();
        let generation = self.parameters.restore_generation();
        let command = self.parameters.with_editing(|editing, _| editing.replay(undo));
        if let Some(command) = command {
            let targets = command
                .changes
                .into_iter()
                .map(|(id, before, after)| (id, if undo { before } else { after }))
                .collect::<Vec<_>>();
            self.recall_values(&targets, generation, false);
            if generation == self.parameters.restore_generation() {
                self.parameters
                    .with_editing(|editing, snapshot| editing.complete_replay(undo, snapshot));
            }
            self.presets.invalidate();
            if self.binding.is_open() && generation == self.parameters.restore_generation() {
                self.binding.mark_dirty();
            }
        }
    }

    /// Switching captures outgoing headless automation and recalls sound only as one chronological command.
    pub(super) fn switch_comparison(&mut self, slot: usize) {
        self.finish_numeric();
        self.end_gesture();
        let generation = self.parameters.restore_generation();
        self.presets.invalidate();
        let target = self.parameters.with_editing(|editing, snapshot| {
            editing.begin();
            editing.switch(slot, snapshot)
        });
        if let Some(sound) = target {
            self.recall_values(&sound.parameters().collect::<Vec<_>>(), generation, true);
        }
        if target.is_some() && generation == self.parameters.restore_generation() {
            self.parameters.with_editing(|editing, snapshot| {
                let current = crate::editing::SoundSnapshot::capture(snapshot);
                let comparison = editing.comparison(snapshot);
                if let Some(origin) = comparison.origins.get(comparison.active).and_then(Option::as_ref)
                    && target == Some(origin.effective.unwrap_or(origin.baseline))
                {
                    editing.set_effective_origin(current);
                }
            });
        }
        self.parameters.with_editing(crate::editing::EditingState::end);
        if target.is_some() && self.binding.is_open() && generation == self.parameters.restore_generation() {
            self.binding.mark_dirty();
        }
    }

    /// Copy changes only the other slot's sound and is undoable even though live parameters stay unchanged.
    pub(super) fn copy_comparison(&mut self) {
        self.finish_numeric();
        self.end_gesture();
        self.parameters.with_editing(|editing, snapshot| {
            editing.begin();
            editing.copy(snapshot);
            editing.end(snapshot);
        });
        self.binding.mark_dirty();
    }

    /// Sends independently balanced host gestures and stops stale work after every potentially reentrant callback.
    fn recall_values(&self, targets: &[(u32, f64)], generation: u64, record: bool) {
        for &(id, value) in targets {
            if !self.binding.is_open() || generation != self.parameters.restore_generation() {
                break;
            }
            if self
                .parameters
                .get(id)
                .is_some_and(|current| current.to_bits() == value.to_bits())
            {
                continue;
            }
            self.binding.begin_edit(id);
            if record {
                self.write(id, value, generation);
            } else if self.binding.is_open() && generation == self.parameters.restore_generation() {
                self.binding.publish_value(&self.parameters, id, value);
            }
            self.binding.end_edit(id);
        }
    }
}
