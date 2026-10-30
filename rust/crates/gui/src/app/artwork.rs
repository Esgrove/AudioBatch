//! Selected-file artwork requests, independent of cue readiness and analysis.
//!
//! One background read runs at a time and only the latest pending selection is retained.
//! Generation and file identity checks reject obsolete work before reading, before decoding, and on adoption.
//! Completed identities include missing artwork, so information refreshes do not repeatedly read empty tags.

use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::sync::atomic::{AtomicU64, Ordering};

use audiobatch_core::record::file_identity;
use gpui_kit::{Context, RenderImage};

use super::AppView;
use crate::artwork::load_artwork_thumbnail_if_current;

/// Selected path and the source identity used by one artwork read.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(super) struct ArtworkIdentity {
    path: PathBuf,
    file_size: u64,
    modified_time_ms: i64,
}

/// A background request whose generation can be invalidated without waiting for its reader.
#[derive(Clone)]
struct ArtworkRequest {
    identity: ArtworkIdentity,
    generation: u64,
    current_generation: Arc<AtomicU64>,
}

/// The active read, latest desired read, and completed identity, including a completed no-art result.
#[derive(Default)]
pub(super) struct ArtworkRequests {
    generation: Arc<AtomicU64>,
    desired: Option<ArtworkIdentity>,
    active: Option<ArtworkRequest>,
    pending: Option<ArtworkRequest>,
    completed: Option<ArtworkIdentity>,
}

impl ArtworkIdentity {
    /// Captures an existing file independently of its analysis or playback availability.
    fn read(path: &Path) -> Option<Self> {
        let (file_size, modified_time_ms) = file_identity(path)?;
        Some(Self {
            path: path.to_path_buf(),
            file_size,
            modified_time_ms,
        })
    }

    /// Returns whether this identity still describes the bytes at its path.
    fn matches_disk(&self) -> bool {
        file_identity(&self.path) == Some((self.file_size, self.modified_time_ms))
    }
}

impl ArtworkRequest {
    /// Rejects a replaced generation or changed source before a costly background stage.
    fn is_current(&self) -> bool {
        self.current_generation.load(Ordering::Acquire) == self.generation && self.identity.matches_disk()
    }
}

impl ArtworkRequests {
    /// Changes the desired identity without interrupting the active reader or queueing intermediate selections.
    /// Returns false for a repeated request, including an identity already known to have no artwork.
    fn select(&mut self, identity: Option<ArtworkIdentity>) -> bool {
        if self.desired == identity {
            return false;
        }
        let generation = self.generation.fetch_add(1, Ordering::AcqRel).wrapping_add(1);
        self.desired.clone_from(&identity);
        self.completed = None;
        self.pending = identity.map(|identity| ArtworkRequest {
            identity,
            generation,
            current_generation: Arc::clone(&self.generation),
        });
        true
    }

    /// Starts the latest request only after the preceding background job has returned.
    fn take_next(&mut self) -> Option<ArtworkRequest> {
        if self.active.is_some() {
            return None;
        }
        let request = self.pending.take()?;
        self.active = Some(request.clone());
        Some(request)
    }

    /// Completes a read and remembers valid results, even when decoding found no artwork.
    /// The caller separately checks the current on-disk identity before adopting an image.
    fn complete(&mut self, request: &ArtworkRequest, matches_disk: bool) -> bool {
        if self.active.as_ref().map(|active| active.generation) != Some(request.generation) {
            return false;
        }
        self.active = None;
        let current = self.generation.load(Ordering::Acquire) == request.generation
            && self.desired.as_ref() == Some(&request.identity)
            && matches_disk;
        if current {
            self.completed = Some(request.identity.clone());
        } else if self.desired.as_ref() == Some(&request.identity) && self.pending.is_none() {
            // A same-path replacement during the read must be eligible for a fresh request.
            self.desired = None;
        }
        current
    }

    /// Invalidates active stages and forgets completed no-art results for a forced refresh or release.
    pub(super) fn invalidate(&mut self) {
        self.generation.fetch_add(1, Ordering::AcqRel);
        self.desired = None;
        self.pending = None;
        self.completed = None;
    }
}

impl Drop for ArtworkRequests {
    fn drop(&mut self) {
        self.invalidate();
    }
}

impl AppView {
    /// Requests artwork independently of audio load success, coalescing repeated identities and pending selections.
    pub(super) fn request_artwork(&mut self, path: Option<&Path>, cx: &mut Context<Self>) {
        let identity = path
            .filter(|path| self.settings.show_album_artwork && !self.artwork_rewrites.contains(*path))
            .and_then(ArtworkIdentity::read);
        if self.artwork_requests.select(identity) {
            self.set_artwork(None, cx);
        }
        self.start_pending_artwork(cx);
    }

    /// Returns artwork only while its path, identity, and enabled preference still match the selected source.
    pub(super) fn selected_artwork(&self) -> Option<Arc<RenderImage>> {
        self.selected_artwork
            .as_ref()
            .filter(|(identity, _)| {
                self.settings.show_album_artwork
                    && self.selected.as_ref() == Some(&identity.path)
                    && identity.matches_disk()
            })
            .map(|(_, image)| Arc::clone(image))
    }

    /// Rechecks the selected source after an information refresh, retaining completed no-art results by identity.
    pub(super) fn refresh_artwork(&mut self, cx: &mut Context<Self>) {
        let selected = self.selected.clone();
        self.request_artwork(selected.as_deref(), cx);
    }

    /// Clears artwork and invalidates pending reads on preview release or forced source refresh.
    pub(super) fn invalidate_artwork(&mut self, cx: &mut Context<Self>) {
        self.artwork_requests.invalidate();
        self.set_artwork(None, cx);
    }

    /// Runs one read to completion before starting the latest pending identity.
    fn start_pending_artwork(&mut self, cx: &Context<Self>) {
        let Some(request) = self.artwork_requests.take_next() else {
            return;
        };
        self.artwork_task = Some(cx.spawn(async move |this, cx| {
            let read_request = request.clone();
            let thumbnail = cx
                .background_executor()
                .spawn(async move {
                    load_artwork_thumbnail_if_current(&read_request.identity.path, || read_request.is_current())
                })
                .await;
            let _ = this.update(cx, |view, cx| {
                let matches_disk = request.identity.matches_disk();
                let changed_source = !matches_disk && view.artwork_requests.desired.as_ref() == Some(&request.identity);
                let current = view.artwork_requests.complete(&request, matches_disk);
                if current && view.settings.show_album_artwork {
                    view.set_artwork(thumbnail.map(|image| (request.identity, Arc::new(image))), cx);
                }
                if changed_source {
                    view.refresh_artwork(cx);
                } else {
                    view.start_pending_artwork(cx);
                }
            });
        }));
    }

    /// Replaces the thumbnail and frees its prior texture after the current update releases window borrows.
    fn set_artwork(&mut self, artwork: Option<(ArtworkIdentity, Arc<RenderImage>)>, cx: &mut Context<Self>) {
        let previous = std::mem::replace(&mut self.selected_artwork, artwork);
        if let Some((_, image)) = previous {
            cx.defer(move |cx| cx.drop_image(image, None));
        }
        cx.notify();
    }
}

#[cfg(test)]
mod test_artwork_requests {
    use super::*;

    /// Supplies deterministic source identities without filesystem timing or GUI entities.
    fn identity(path: &str, file_size: u64) -> ArtworkIdentity {
        ArtworkIdentity {
            path: path.into(),
            file_size,
            modified_time_ms: 10,
        }
    }

    #[test]
    fn rapid_a_b_a_keeps_one_active_and_only_the_latest_pending_request() {
        let mut state = ArtworkRequests::default();
        state.select(Some(identity("a", 1)));
        let first = state.take_next().expect("first request");
        state.select(Some(identity("b", 2)));
        state.select(Some(identity("a", 1)));
        assert!(state.take_next().is_none());
        assert!(!state.complete(&first, true));
        let last = state.take_next().expect("latest request");
        assert_eq!(last.identity.path, Path::new("a"));
        assert_ne!(first.generation, last.generation);
        assert!(state.complete(&last, true));
        assert!(state.take_next().is_none());
    }

    #[test]
    fn disabling_or_clearing_rejects_a_completion_and_pending_work() {
        let mut state = ArtworkRequests::default();
        state.select(Some(identity("a", 1)));
        let active = state.take_next().expect("active request");
        state.select(Some(identity("b", 2)));
        state.select(None);
        assert!(!state.complete(&active, true));
        assert!(state.take_next().is_none());
        state.select(Some(identity("a", 1)));
        assert!(state.take_next().is_some());
    }

    #[test]
    fn no_art_completion_is_remembered_until_identity_changes_or_forced_refresh() {
        let mut state = ArtworkRequests::default();
        let source = identity("a", 1);
        state.select(Some(source.clone()));
        let request = state.take_next().expect("request");
        assert!(state.complete(&request, true));
        assert_eq!(state.completed.as_ref(), Some(&source));
        assert!(!state.select(Some(source.clone())));
        assert!(state.take_next().is_none());
        state.invalidate();
        assert!(state.select(Some(source)));
        let refreshed = state.take_next().expect("forced read");
        state.select(Some(identity("a", 2)));
        assert!(!state.complete(&refreshed, true));
        assert_eq!(state.take_next().expect("replacement read").identity.file_size, 2);
    }

    #[test]
    fn changed_identity_during_read_is_not_cached_as_a_missing_picture() {
        let mut state = ArtworkRequests::default();
        let source = identity("a", 1);
        state.select(Some(source.clone()));
        let request = state.take_next().expect("request");
        assert!(!state.complete(&request, false));
        assert!(state.completed.is_none());
        assert!(state.select(Some(source)));
        assert!(state.take_next().is_some());
    }

    #[test]
    fn shutdown_and_invalidation_cancel_stage_tokens_without_dropping_the_active_read() {
        let mut state = ArtworkRequests::default();
        state.select(Some(identity("a", 1)));
        let request = state.take_next().expect("request");
        let generation = Arc::clone(&request.current_generation);
        state.invalidate();
        assert_ne!(generation.load(Ordering::Acquire), request.generation);
        assert!(state.take_next().is_none());
        drop(state);
        assert_ne!(generation.load(Ordering::Acquire), request.generation);
    }
}
