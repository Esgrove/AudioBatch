//! The tags of the selected file: the information panel fields, the Serato cue points, and the Serato Overview.
//!
//! A new selection first shows the Serato Overview kept in the cache, so the waveform is coloured right away,
//! then reads the tags on a background thread.
//! The read result replaces the cached overview, and a changed result is written back to the cache.
//! A failed read keeps what the cache had and is never cached, because the error may be temporary.

use std::path::{Path, PathBuf};
use std::sync::Arc;

use audiobatch_core::record::file_identity;
use audiobatch_core::serato_tags::Overview;
use audiobatch_core::tags::read_display_tags;
use audiobatch_core::{FileTags, SeratoOverviewState};
use gpui_kit::{Context, Task};

use super::AppView;
use crate::waveform::CueMarker;

impl AppView {
    /// Reads the tags of a file on a background thread and shows them if it is still selected.
    /// The Serato cue points of the file are drawn on the waveform,
    /// and the file moves to its first cue point when that option is on.
    /// The Serato Overview colours the waveform, first from the cache and then from the tags.
    pub(super) fn spawn_tag_read(&self, path: PathBuf, cx: &Context<Self>) -> Task<()> {
        let cache = self
            .coordinator
            .as_ref()
            .map(|coordinator| Arc::clone(coordinator.cache()));
        cx.spawn(async move |this, cx| {
            let cached = match cache.clone() {
                Some(cache) => {
                    let lookup_path = path.clone();
                    cx.background_executor()
                        .spawn(async move { cache.get_serato_overview(&lookup_path) })
                        .await
                }
                None => None,
            };
            if let Some(state) = &cached {
                let overview = state.overview().cloned();
                if this
                    .update(cx, |view, cx| view.show_serato_overview(&path, overview, cx))
                    .is_err()
                {
                    return;
                }
            }
            let read_path = path.clone();
            let (tags, state) = cx
                .background_executor()
                .spawn(async move {
                    let identity = file_identity(&read_path);
                    let tags = read_display_tags(&read_path).ok();
                    let state = tags
                        .as_ref()
                        .map(|tags| SeratoOverviewState::from_tags(&read_path, tags));
                    // A file rewritten while its tags were read would cache the old overview under its new identity.
                    if let (Some(cache), Some(state)) = (&cache, &state)
                        && cached.as_ref() != Some(state)
                        && identity.is_some()
                        && file_identity(&read_path) == identity
                        && let Err(error) = cache.store_serato_overview(&read_path, state)
                    {
                        let error = anyhow::Error::from(error);
                        tracing::warn!("Failed to cache the Serato tags of {}: {error:#}", read_path.display());
                    }
                    (tags, state)
                })
                .await;
            let _ = this.update(cx, |view, cx| {
                if view.selected.as_deref() != Some(path.as_path()) {
                    return;
                }
                if let Some(state) = &state {
                    view.show_serato_overview(&path, state.overview().cloned(), cx);
                }
                let cues = tags.as_ref().map(cue_markers).unwrap_or_default();
                view.start_at_first_cue(&path, &cues, cx);
                view.waveform.update(cx, |waveform, cx| waveform.set_cues(cues, cx));
                view.selected_tags = tags.map(|tags| (path, Arc::new(tags)));
                cx.notify();
            });
        })
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

    /// Colours the waveform with a Serato Overview when the file is still selected.
    fn show_serato_overview(&self, path: &Path, overview: Option<Arc<Overview>>, cx: &mut Context<Self>) {
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
