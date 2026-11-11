//! The per-file custom gain of the selected files: changing it from the slider or the mouse wheel,
//! writing it to the analysis cache once the change is finished, and applying it to the preview and the waveform.

use std::collections::HashSet;
use std::path::{Path, PathBuf};
use std::time::Duration;

use audiobatch_core::AnalysisRecord;
use audiobatch_core::record::{custom_gain_from_decibels, decibels_to_gain};
use audiobatch_playback::Player;
use gpui_kit::{App, Context, Window};

use super::AppView;
use super::preview::PreviewSource;
use super::selection::presentation_matches_source;
use crate::results::ResultsDelegate;

/// Lowest input gain in decibels.
pub(super) const MINIMUM_GAIN_DB: f32 = -12.0;

/// Highest input gain in decibels.
pub(super) const MAXIMUM_GAIN_DB: f32 = 12.0;

/// Input gain at startup and after a reset, in decibels.
pub(super) const DEFAULT_GAIN_DB: f32 = 0.0;

/// Difference between the gain slider and the selected file's gain that moves the slider, in decibels.
const GAIN_SLIDER_TOLERANCE_DB: f32 = 1e-3;

/// How long gain edits must rest before their latest values are written to the cache.
const GAIN_STORE_DELAY: Duration = Duration::from_millis(500);

/// Independent linear gains authorized for decoded audio and static waveform presentation.
struct PreviewGains {
    audible: f32,
    display: f32,
}

impl AppView {
    /// Clears the custom gain of the selected files, in the table and in the cache.
    pub(super) fn clear_selected_gain(&mut self, cx: &mut Context<Self>) {
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
        // The gain applies to every selected file.
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

    /// Commits explicit selected and pending gain edits immediately, without a later timer submission.
    pub(super) fn store_selected_gain(&mut self, cx: &mut Context<Self>) {
        self.flush_gain_store(cx);
    }

    /// Coalesces slider and wheel edits until they rest, retaining their original paths across selection changes.
    /// Previously pending paths outside the new selection are submitted before starting its debounce.
    pub(super) fn schedule_gain_store(&mut self, cx: &mut Context<Self>) {
        let selected = self.selected_paths(cx);
        if selected.is_empty() {
            return;
        }
        if self.pending_gain_paths != selected {
            // Overlapping paths already contain this event's latest value, so keep those on the new deadline.
            let previous: Vec<_> = std::mem::take(&mut self.pending_gain_paths)
                .into_iter()
                .filter(|path| !selected.contains(path))
                .collect();
            self.persist_gain_edits(&previous, cx);
        }
        self.pending_gain_paths = selected;
        self.gain_store_task = Some(cx.spawn(async move |this, cx| {
            cx.background_executor().timer(GAIN_STORE_DELAY).await;
            let _ = this.update(cx, Self::flush_gain_store);
        }));
    }

    /// Cancels the debounce and submits pending and selected explicit edits before their rows can be discarded.
    pub(super) fn flush_gain_store(&mut self, cx: &mut Context<Self>) {
        self.gain_store_task = None;
        let mut paths = std::mem::take(&mut self.pending_gain_paths);
        for path in self.selected_paths(cx) {
            if !paths.contains(&path) {
                paths.push(path);
            }
        }
        self.persist_gain_edits(&paths, cx);
    }

    /// Submits edits of rows a scan will discard, leaving retained rows on their original debounce.
    /// Retained pending edits must still override analysis results captured before their writer submission.
    pub(super) fn store_unlisted_gains(&mut self, files: &[PathBuf], cx: &mut Context<Self>) {
        let listed: HashSet<_> = files.iter().filter(|path| !self.excluded.contains(*path)).collect();
        let mut removed = Vec::new();
        self.pending_gain_paths.retain(|path| {
            if listed.contains(path) {
                true
            } else {
                removed.push(path.clone());
                false
            }
        });
        self.persist_gain_edits(&removed, cx);
        if self.pending_gain_paths.is_empty() {
            self.gain_store_task = None;
        }
    }

    /// Moves only new explicit edits to an ordered worker using their already captured source identity.
    fn persist_gain_edits(&self, paths: &[PathBuf], cx: &mut Context<Self>) {
        let writes = self.table.update(cx, |state, _| {
            paths
                .iter()
                .filter_map(|path| state.delegate_mut().take_gain_write(path))
                .collect::<Vec<_>>()
        });
        if let Some(writer) = &self.gain_writer {
            for write in writes {
                writer.enqueue(write);
            }
        }
    }

    /// Applies gain only to matching source analysis, retaining static waveform gain without a decoder.
    /// An absent or mismatched loaded source gets unity audible gain instead of a different file's override.
    /// The player applies it only in front of the plugin chain, so a plain preview plays the file as it is.
    pub(super) fn apply_preview_gain(&self, cx: &mut Context<Self>) {
        let PreviewGains { audible, display } = preview_gains(
            &self.preview_source,
            self.player.as_ref().and_then(Player::loaded_path),
            self.selected_record(cx),
        );
        if let Some(player) = &self.player {
            player.set_insert_gain(audible);
        }
        self.waveform
            .update(cx, |waveform, cx| waveform.set_display_gain(display, cx));
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

/// Takes the latest authoritative gain values before the pending rows can be removed.
/// A nullable gain is an explicit clear, not an absent write.
pub(super) fn take_pending_gains(
    delegate: &ResultsDelegate,
    pending: &mut Vec<PathBuf>,
) -> Vec<(PathBuf, Option<f32>)> {
    std::mem::take(pending)
        .into_iter()
        .filter_map(|path| {
            let record = delegate.row_for(&path)?.ready_record()?;
            record.matches_file_on_disk().then_some((path, record.custom_gain_db))
        })
        .collect()
}

/// Separates audible authority from static display gain when no decoder is loaded.
fn preview_gains(source: &PreviewSource, loaded_path: Option<&Path>, record: Option<&AnalysisRecord>) -> PreviewGains {
    let Some(record) = record else {
        return PreviewGains {
            audible: 1.0,
            display: 1.0,
        };
    };
    let display = if presentation_matches_source(source, loaded_path, record) {
        record.custom_gain_linear()
    } else {
        1.0
    };
    let audible = if loaded_path == Some(record.path.as_path()) && source.matches_record(record) {
        record.custom_gain_linear()
    } else {
        1.0
    };
    PreviewGains { audible, display }
}

#[cfg(test)]
mod test_preview_gain_authority {
    use std::io::Write as _;

    use audiobatch_core::AnalysisStatus;
    use audiobatch_core::record::file_identity;

    use super::*;

    /// Makes a ready record for the actual source bytes without opening an audio device.
    fn record(path: &Path) -> AnalysisRecord {
        let audiobatch_core::record::FileIdentity {
            file_size,
            modified_time_ms,
        } = file_identity(path).expect("source identity");
        AnalysisRecord {
            path: path.to_path_buf(),
            file_size,
            modified_time_ms,
            status: AnalysisStatus::Analyzed,
            custom_gain_db: Some(-6.0),
            ..AnalysisRecord::default()
        }
    }

    #[test]
    fn matching_loaded_source_applies_gain_to_audio_and_display() {
        let file = tempfile::NamedTempFile::new().expect("source");
        let record = record(file.path());
        let mut source = PreviewSource::default();
        source.begin(file.path());
        let PreviewGains { audible, display } = preview_gains(&source, Some(file.path()), Some(&record));
        assert!((audible - record.custom_gain_linear()).abs() < f32::EPSILON);
        assert!((display - audible).abs() < f32::EPSILON);
    }

    #[test]
    fn missing_decoder_keeps_static_gain_but_no_audible_override() {
        let file = tempfile::NamedTempFile::new().expect("source");
        let record = record(file.path());
        let source = PreviewSource::default();
        let PreviewGains { audible, display } = preview_gains(&source, None, Some(&record));
        assert!((audible - 1.0).abs() < f32::EPSILON);
        assert!((display - record.custom_gain_linear()).abs() < f32::EPSILON);
    }

    #[test]
    fn changed_identity_cannot_apply_old_gain_or_borrow_new_analysis() {
        let mut file = tempfile::NamedTempFile::new().expect("source");
        let old_record = record(file.path());
        let mut source = PreviewSource::default();
        source.begin(file.path());
        file.write_all(b"replacement").expect("replacement bytes");
        let current_record = record(file.path());
        for record in [&old_record, &current_record] {
            let PreviewGains { audible, display } = preview_gains(&source, Some(file.path()), Some(record));
            assert!((audible - 1.0).abs() < f32::EPSILON);
            assert!((display - 1.0).abs() < f32::EPSILON);
        }
        source.begin(file.path());
        let audible = preview_gains(&source, Some(file.path()), Some(&current_record)).audible;
        assert!((audible - current_record.custom_gain_linear()).abs() < f32::EPSILON);
    }

    #[test]
    fn wrong_path_and_cleared_source_reset_audible_gain() {
        let file = tempfile::NamedTempFile::new().expect("source");
        let record = record(file.path());
        let mut source = PreviewSource::default();
        source.begin(file.path());
        let wrong_path_gain = preview_gains(&source, Some(Path::new("another.wav")), Some(&record)).audible;
        assert!((wrong_path_gain - 1.0).abs() < f32::EPSILON);
        source.clear();
        let cleared_gain = preview_gains(&source, Some(file.path()), Some(&record)).audible;
        assert!((cleared_gain - 1.0).abs() < f32::EPSILON);
    }
}
