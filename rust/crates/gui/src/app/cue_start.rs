//! A cue-delayed play intent, retained until both matching metadata and source duration are ready.
//! Taking or replacing the intent makes Stop, seek, unload, and a later load authoritative over late callbacks.

use std::ops::Deref;
use std::path::{Path, PathBuf};

/// One load's request to begin playback after its cue metadata is known.
#[derive(Debug)]
pub(super) struct CueStartIntent {
    path: PathBuf,
    cues: Option<Vec<f64>>,
    play: bool,
}

impl CueStartIntent {
    /// Starts waiting for metadata of the newly selected source.
    pub(super) fn new(path: &Path) -> Self {
        Self {
            path: path.to_path_buf(),
            cues: None,
            play: true,
        }
    }

    /// Starts a cue-position request without playback for a newly selected, paused source.
    pub(super) fn seek(path: &Path) -> Self {
        Self {
            play: false,
            ..Self::new(path)
        }
    }

    /// Reports whether the pending intent should begin playback rather than only position the source.
    pub(super) const fn should_play(&self) -> bool {
        self.play
    }

    /// Stores matching metadata, including an empty list when the file has no cues.
    pub(super) fn receive_cues(&mut self, path: &Path, cues: impl Iterator<Item = f64>) {
        if self.path == path {
            self.cues = Some(cues.collect());
        }
    }

    /// Resolves only after matching metadata and a positive finite duration are ready.
    pub(super) fn position(&self, loaded: Option<&Path>, duration: Option<f64>) -> Option<f64> {
        if loaded != Some(self.path.as_path()) {
            return None;
        }
        let cues = self.cues.as_ref()?;
        let duration = duration.filter(|duration| duration.is_finite() && *duration > 0.0)?;
        Some(
            cues.iter()
                .copied()
                .filter(|seconds| seconds.is_finite() && *seconds >= 0.0)
                .min_by(f64::total_cmp)
                .filter(|seconds| *seconds <= duration * 0.5)
                .unwrap_or(0.0),
        )
    }
}

impl Deref for CueStartIntent {
    type Target = Path;

    fn deref(&self) -> &Self::Target {
        &self.path
    }
}

#[cfg(test)]
mod test_cue_readiness {
    use super::*;

    #[test]
    fn both_completion_orders_resolve_the_same_position() {
        let path = Path::new("song.wav");
        let mut intent = CueStartIntent::new(path);
        assert_eq!(intent.position(Some(path), Some(30.0)), None);
        intent.receive_cues(path, [4.0, 2.0].into_iter());
        assert_eq!(intent.position(Some(path), None), None);
        assert_eq!(intent.position(Some(path), Some(30.0)), Some(2.0));
    }

    #[test]
    fn wrong_source_and_unready_duration_never_consume_the_request() {
        let path = Path::new("song.wav");
        let mut intent = CueStartIntent::new(path);
        intent.receive_cues(Path::new("old.wav"), std::iter::once(2.0));
        assert_eq!(intent.position(Some(path), Some(30.0)), None);
        intent.receive_cues(path, std::iter::once(2.0));
        assert_eq!(intent.position(Some(Path::new("other.wav")), Some(30.0)), None);
        for duration in [0.0, -1.0, f64::NAN, f64::INFINITY] {
            assert_eq!(intent.position(Some(path), Some(duration)), None);
        }
    }

    #[test]
    fn paused_selection_only_positions_without_requesting_playback() {
        let path = Path::new("song.wav");
        let mut intent = CueStartIntent::seek(path);
        assert!(!intent.should_play());
        intent.receive_cues(path, std::iter::once(4.0));
        assert_eq!(intent.position(Some(path), Some(30.0)), Some(4.0));
        assert!(CueStartIntent::new(path).should_play());
    }

    #[test]
    fn empty_or_late_cues_start_at_zero_after_readiness() {
        let path = Path::new("song.wav");
        let mut intent = CueStartIntent::new(path);
        intent.receive_cues(path, std::iter::empty());
        assert_eq!(intent.position(Some(path), Some(30.0)), Some(0.0));
        intent.receive_cues(path, std::iter::once(25.0));
        assert_eq!(intent.position(Some(path), Some(30.0)), Some(0.0));
    }

    #[test]
    fn superseding_an_intent_discards_late_metadata() {
        let path = Path::new("song.wav");
        let mut pending = Some(CueStartIntent::new(path));
        assert!(pending.take().is_some());
        if let Some(intent) = &mut pending {
            intent.receive_cues(path, std::iter::once(2.0));
        }
        assert!(pending.is_none());
    }
}
