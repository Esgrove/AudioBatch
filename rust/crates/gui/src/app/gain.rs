//! The per-file custom gain of the selected files: changing it from the slider or the mouse wheel,
//! writing it to the analysis cache once the change is finished, and applying it to the preview and the waveform.

use std::path::Path;
use std::time::Duration;

use audiobatch_core::AnalysisRecord;
use audiobatch_core::record::{custom_gain_from_decibels, decibels_to_gain};
use gpui_kit::{App, Context, Window};

use super::AppView;

/// Lowest input gain in decibels.
pub(super) const MINIMUM_GAIN_DB: f32 = -12.0;

/// Highest input gain in decibels.
pub(super) const MAXIMUM_GAIN_DB: f32 = 12.0;

/// Input gain at startup and after a reset, in decibels.
pub(super) const DEFAULT_GAIN_DB: f32 = 0.0;

/// Difference between the gain slider and the selected file's gain that moves the slider, in decibels.
const GAIN_SLIDER_TOLERANCE_DB: f32 = 1e-3;

/// Gain change of one wheel event over the waveform in decibels, the step of the gain slider.
pub(super) const GAIN_WHEEL_STEP_DB: f32 = 0.1;

/// How long the mouse wheel has to rest before a gain it changed is written to the cache.
const GAIN_STORE_DELAY: Duration = Duration::from_millis(500);

impl AppView {
    /// Clears the custom gain of the selected files, in the table and in the cache.
    pub(super) fn clear_selected_gain(&self, cx: &mut Context<Self>) {
        self.change_selected_gain(DEFAULT_GAIN_DB, cx);
        self.store_selected_gain(cx);
        cx.notify();
    }

    /// Returns true when the selected file has a usable result, so its custom gain can be changed,
    /// or while the stress test runs, where the gain only scales the drawn waveform.
    pub(super) fn can_adjust_gain(&self, cx: &App) -> bool {
        self.is_demo() || self.selected_record(cx).is_some()
    }

    /// Sets the custom gain of the selected file while the slider or the mouse wheel moves,
    /// and applies it to the preview and the drawn waveform.
    /// The cache is written by `store_selected_gain` once the change is finished.
    /// In the stress test the gain only scales the drawn waveform.
    pub(super) fn change_selected_gain(&self, decibels: f32, cx: &mut Context<Self>) {
        if self.is_demo() {
            let linear = decibels_to_gain(f64::from(decibels)) as f32;
            self.waveform
                .update(cx, |waveform, cx| waveform.set_display_gain(linear, cx));
            return;
        }
        let selected = self.selected_paths(cx);
        if selected.is_empty() {
            return;
        }
        // Like the C++ application, the gain applies to every selected file.
        let gain = custom_gain_from_decibels(decibels);
        let changed = self.table.update(cx, |state, cx| {
            let delegate = state.delegate_mut();
            let mut changed = false;
            for path in &selected {
                changed |= delegate.set_custom_gain(path, gain);
            }
            if changed {
                cx.notify();
            }
            changed
        });
        if changed {
            self.apply_preview_gain(cx);
        }
    }

    /// Stores the custom gain of the selected files in the cache.
    pub(super) fn store_selected_gain(&self, cx: &App) {
        for record in self.selected_ready_records(cx) {
            self.store_custom_gain(&record.path, record.custom_gain_db);
        }
    }

    /// Writes the custom gain of the selected file to the cache once the mouse wheel rests,
    /// so a wheel gesture does not write to the database on the UI thread for every step.
    /// A pending gain of another file is written right away.
    pub(super) fn schedule_gain_store(&mut self, cx: &Context<Self>) {
        let selected = self.selected_paths(cx);
        if selected.is_empty() {
            return;
        }
        if self.pending_gain_paths != selected {
            self.flush_gain_store(cx);
        }
        self.pending_gain_paths = selected;
        self.gain_store_task = Some(cx.spawn(async move |this, cx| {
            cx.background_executor().timer(GAIN_STORE_DELAY).await;
            let _ = this.update(cx, |view, cx| view.flush_gain_store(cx));
        }));
    }

    /// Writes the current custom gain of the files the mouse wheel changed, if any, to the cache.
    pub(super) fn flush_gain_store(&mut self, cx: &App) {
        let paths = std::mem::take(&mut self.pending_gain_paths);
        let delegate = self.table.read(cx).delegate();
        for path in paths {
            let gain = delegate
                .row_for(&path)
                .and_then(|row| row.ready_record())
                .map(|record| record.custom_gain_db);
            if let Some(gain) = gain {
                self.store_custom_gain(&path, gain);
            }
        }
    }

    /// Writes the custom gain of a file to the cache.
    /// A failure is only reported, because the table keeps the gain for this session either way.
    pub(super) fn store_custom_gain(&self, path: &Path, custom_gain_db: Option<f32>) {
        if let Some(coordinator) = &self.coordinator
            && let Err(error) = coordinator.cache().store_custom_gain(path, custom_gain_db)
        {
            tracing::warn!("Failed to store the custom gain of {}: {error:#}", path.display());
        }
    }

    /// Applies the custom gain of the selected file to the preview and to the drawn waveform,
    /// or unity gain when it has none.
    /// The player applies it only in front of the plugin chain, so a plain preview plays the file as it is.
    pub(super) fn apply_preview_gain(&self, cx: &mut Context<Self>) {
        let linear = self.selected_record(cx).map_or(1.0, AnalysisRecord::custom_gain_linear);
        if let Some(player) = &self.player {
            player.set_insert_gain(linear);
        }
        self.waveform
            .update(cx, |waveform, cx| waveform.set_display_gain(linear, cx));
    }

    /// Moves the gain slider to the custom gain of the selected file, 0 dB without one.
    /// The record is the source of truth.
    /// A new selection, a cleared gain, and a gain loaded from the cache all show on the slider this way,
    /// without each of them having to update it.
    pub(super) fn sync_gain_slider(&self, window: &mut Window, cx: &mut Context<Self>) {
        if self.is_demo() {
            return;
        }
        let target = self
            .selected_record(cx)
            .and_then(|record| record.custom_gain_db)
            .unwrap_or(DEFAULT_GAIN_DB);
        let current = self.gain_slider.read(cx).value().start();
        if (current - target).abs() >= GAIN_SLIDER_TOLERANCE_DB {
            self.gain_slider
                .update(cx, |slider, cx| slider.set_value(target, window, cx));
        }
    }
}
