//! Selected-file display tags, cue readiness, and Serato colours.
//!
//! Reads are admitted by the shared enrichment worker, never by an independent selected-file pool.

use std::path::{Path, PathBuf};
use std::sync::Arc;

use audiobatch_core::serato_tags::Overview;
use audiobatch_core::{FileTags, SeratoOverviewState};
use gpui_kit::Context;

use super::AppView;
use crate::waveform::CueMarker;

impl AppView {
    /// Applies an identity-checked display read and may fulfill a pending cue-start playback intent.
    /// An already consumed cue intent is never replayed.
    pub(super) fn adopt_display_tags(&mut self, path: &Path, tags: Option<Arc<FileTags>>, cx: &mut Context<Self>) {
        if self.selected.as_deref() != Some(path) {
            return;
        }
        if let Some(tags) = &tags {
            let state = SeratoOverviewState::from_tags(path, tags);
            self.show_serato_overview(path, state.overview().cloned(), cx);
        }
        let cues = tags.as_deref().map(cue_markers).unwrap_or_default();
        self.start_at_first_cue(path, &cues, cx);
        self.waveform.update(cx, |waveform, cx| waveform.set_cues(cues, cx));
        self.selected_tags = tags.map(|tags| (PathBuf::from(path), tags));
        cx.notify();
    }

    /// Plays the selected file from its Serato hot cue in a slot, from the keys 1 to 8.
    /// Nothing happens when the file has no cue in that slot.
    pub(super) fn jump_to_hot_cue(&mut self, slot: u8, cx: &mut Context<Self>) {
        let Some((path, tags)) = &self.selected_tags else {
            return;
        };
        let Some(seconds) = cue_markers(tags)
            .into_iter()
            .find(|cue| cue.slot == slot)
            .map(|cue| cue.seconds)
        else {
            return;
        };
        self.start_after_cues = None;
        if let Some(player) = self
            .player
            .as_mut()
            .filter(|player| player.loaded_path() == Some(path.as_path()))
        {
            player.seek(seconds);
            player.play();
        }
        cx.notify();
    }

    /// Removes the cue points and the Serato colours of the previous selection from the waveform.
    pub(super) fn clear_tag_marks(&self, cx: &mut Context<Self>) {
        self.waveform.update(cx, |waveform, cx| {
            waveform.set_cues(Vec::new(), cx);
            waveform.set_serato_overview(None, cx);
        });
    }

    /// Colours the waveform only after the caller has checked the matching selected-preview generation.
    pub(super) fn show_serato_overview(&self, path: &Path, overview: Option<Arc<Overview>>, cx: &mut Context<Self>) {
        if self.selected.as_deref() == Some(path) {
            self.waveform
                .update(cx, |waveform, cx| waveform.set_serato_overview(overview, cx));
        }
    }
}

/// Returns the Serato cue points of a file as waveform markers.
fn cue_markers(tags: &FileTags) -> Vec<CueMarker> {
    tags.serato
        .as_ref()
        .map(|serato| {
            serato
                .cues()
                .map(|cue| CueMarker {
                    seconds: f64::from(cue.position) / 1000.0,
                    color: cue.color.to_rgb_u32(),
                    slot: cue.index,
                })
                .collect()
        })
        .unwrap_or_default()
}
