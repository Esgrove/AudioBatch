//! The live preview through the plugin chain in the main view.
//!
//! The player gets a new chain whenever the structure of the chain, the preview options, or the instances change,
//! and none while the preview is off or no plugin is enabled.
//! Before instances are prepared again, the chain is taken out of the player,
//! and the view waits until the audio callback has let go of it, so no processor runs during the reconfiguration.
//! The housekeeping tick passes changed tails on to the player
//! and turns the preview off when a plugin failed, reporting the plugin and its error.

use std::path::{Path, PathBuf};
use std::thread;
use std::time::{Duration, Instant};

use audiobatch_core::AnalysisRecord;
use audiobatch_core::record::file_identity;
use audiobatch_playback::Player;
use gpui_kit::Context;
use vst3_host::UiThread;

use super::AppView;
use crate::plugins::preview::ChainInsert;

/// Longest wait for the audio callback to let go of a chain.
/// A running stream takes it within one callback, so only a stopped stream takes this long.
const DETACH_TIMEOUT: Duration = Duration::from_millis(250);

/// How often the wait checks whether the audio callback let go of the chain.
const DETACH_POLL_INTERVAL: Duration = Duration::from_millis(1);

/// A source version shared by decoding, tags and analysis presentation.
#[derive(Debug, Clone, PartialEq, Eq)]
pub(super) struct PreviewVersion {
    path: PathBuf,
    identity: (u64, i64),
}

/// Tracks the source captured before a decoder load and invalidates callbacks across same-path reloads.
#[derive(Debug, Default)]
pub(super) struct PreviewSource {
    version: Option<PreviewVersion>,
    generation: u64,
}

impl PreviewVersion {
    /// Captures the existing source without treating missing metadata as a usable identity.
    fn capture(path: &Path) -> Option<Self> {
        Some(Self {
            path: path.to_path_buf(),
            identity: file_identity(path)?,
        })
    }

    /// Accepts analysis only while it describes this source and the file is still unchanged.
    fn matches_record(&self, record: &AnalysisRecord) -> bool {
        record.is_ready()
            && record.path == self.path
            && (record.file_size, record.modified_time_ms) == self.identity
            && self.matches_disk()
    }

    /// Checks the captured version rather than sampling a new identity at callback publication.
    fn matches_disk(&self) -> bool {
        file_identity(&self.path) == Some(self.identity)
    }
}

impl PreviewSource {
    /// Captures a new source and invalidates all callbacks of the previous load, including same-path retries.
    pub(super) fn begin(&mut self, path: &Path) {
        self.generation = self.generation.wrapping_add(1);
        self.version = PreviewVersion::capture(path);
    }

    /// Releases source authority while retaining static source presentation in its separate owners.
    pub(super) fn clear(&mut self) {
        self.generation = self.generation.wrapping_add(1);
        self.version = None;
    }

    /// Returns an immutable request token for asynchronous tag work.
    pub(super) fn token(&self) -> Option<(PreviewVersion, u64)> {
        Some((self.version.clone()?, self.generation))
    }

    /// Rejects callbacks from an older load even when the path and filesystem metadata are equal.
    pub(super) fn accepts(&self, version: &PreviewVersion, generation: u64) -> bool {
        generation == self.generation && self.version.as_ref() == Some(version) && version.matches_disk()
    }

    /// Reports whether the current decoder belongs to the selected source version.
    pub(super) fn needs_reload(&self, path: &Path, loaded_path: Option<&Path>) -> bool {
        loaded_path != Some(path)
            || self
                .version
                .as_ref()
                .is_none_or(|version| version.path != path || !version.matches_disk())
    }

    /// Authorizes analysis, gain and source warnings against the decoder's captured version.
    pub(super) fn matches_record(&self, record: &AnalysisRecord) -> bool {
        self.version
            .as_ref()
            .is_some_and(|version| version.matches_record(record))
    }
}

impl AppView {
    /// Replaces stale same-path playback and its tags as one load generation before showing new analysis.
    /// An absent player still permits useful static waveform and metadata presentation.
    pub(super) fn reconcile_selected_preview(&mut self, cx: &mut Context<Self>) {
        let Some(path) = self.selected.clone() else {
            return;
        };
        if self.artwork_rewrites.contains(&path) {
            return;
        }
        let loaded_path = self.player.as_ref().and_then(Player::loaded_path);
        if !self.preview_source.needs_reload(&path, loaded_path) {
            return;
        }
        self.preview_source.begin(&path);
        self.start_after_cues = self
            .settings
            .play_from_first_cue_point
            .then(|| super::cue_start::CueStartIntent::seek(&path));
        self.tags_task = None;
        self.selected_tags = None;
        self.clear_tag_marks(cx);
        if let Some(player) = &mut self.player {
            player.load(&path, 0.0, false);
        }
        self.tags_task = Some(self.spawn_tag_read(path, cx));
        self.refresh_selected_waveform(cx);
        self.apply_preview_gain(cx);
    }

    /// Gives the player a new chain of the enabled plugins.
    /// It takes the chain out instead while the preview is off, no plugin is enabled,
    /// or the instances are not yet prepared for the player's rate, which the next tick does.
    /// An enabled plugin that is not ready turns the preview off with its error.
    pub(super) fn refresh_plugin_preview(&mut self, cx: &mut Context<Self>) {
        let Some(ui) = UiThread::current() else {
            return;
        };
        // The enabled plugins load the first time the preview needs them.
        if self.settings.preview_plugin_chain {
            self.plugins.chain.load_slots(&ui, true);
        }
        let Some(player) = &mut self.player else {
            self.plugins.preview.detached();
            return;
        };
        player.set_latency_compensation(self.settings.compensate_preview_latency);
        let chain = if self.settings.preview_plugin_chain && self.plugins.chain.sample_rate() == player.output_rate() {
            self.plugins.chain.preview_chain(&ui)
        } else {
            Ok(None)
        };
        match chain {
            Ok(Some(chain)) => {
                let failure = chain.failure();
                let tail_frames = chain.tail_frames();
                match player.set_insert(Some(Box::new(ChainInsert(chain)))) {
                    Ok(()) => self.plugins.preview.attached(failure, tail_frames),
                    Err(error) => {
                        self.plugins.preview.detached();
                        self.report_status(format!("{error:#}"), cx);
                    }
                }
            }
            Ok(None) => {
                if self.plugins.preview.is_attached() {
                    let _ = player.set_insert(None);
                }
                self.plugins.preview.detached();
            }
            Err(error) => self.turn_plugin_preview_off(error.to_string(), cx),
        }
    }

    /// Takes the chain out of the player and waits until the audio callback has let go of it,
    /// so the instances can be prepared again.
    /// After `DETACH_TIMEOUT` it gives up, which happens only when the stream stopped,
    /// and preparing an instance the callback still holds then fails with a message in the chain window.
    pub(super) fn detach_plugin_preview(&mut self) {
        self.plugins.preview.detached();
        let Some(player) = &mut self.player else {
            return;
        };
        let _ = player.set_insert(None);
        let deadline = Instant::now() + DETACH_TIMEOUT;
        while !player.is_insert_settled() && Instant::now() < deadline {
            thread::sleep(DETACH_POLL_INTERVAL);
        }
        drop(player.take_retired_inserts());
    }

    /// Captures the settings of the live instances into the chain.
    /// The chain is detached and all processors retired before draining edits, even during playback.
    /// A failed capture keeps the previous saved settings and is reported to the operation that requested it.
    pub(super) fn capture_plugin_states(&mut self, cx: &mut Context<Self>) -> anyhow::Result<()> {
        let ui = UiThread::current().ok_or_else(|| anyhow::anyhow!("Plugin settings require the UI thread"))?;
        let attached = self.plugins.preview.is_attached();
        // A finite state snapshot requires exclusive processor ownership, even while playback runs.
        self.detach_plugin_preview();
        let result = self.plugins.chain.capture_states(&ui);
        if attached {
            self.refresh_plugin_preview(cx);
        }
        result
    }

    /// Prepares the live instances again for a new output rate or after their restart requests,
    /// with the chain taken out of the player, and attaches a chain to a replaced player.
    /// Also passes a changed tail on to the player, turns the preview off after a plugin failed,
    /// and drops the chains the audio callback let go of.
    /// Returns true when instances were prepared again.
    pub(super) fn sync_plugin_preview(&mut self, ui: &UiThread, cx: &mut Context<Self>) -> bool {
        let sample_rate = self.preview_sample_rate();
        let rate_changed = sample_rate != self.plugins.chain.sample_rate();
        let restarts = self.plugins.chain.take_restart_requests();
        let prepared = rate_changed || !restarts.is_empty();
        if prepared {
            self.detach_plugin_preview();
            let owner = self.plugins.editor_owner;
            if let Err(error) = self.plugins.chain.apply_restarts(ui, sample_rate, &restarts, owner) {
                self.report_status(format!("Could not restart the plugin chain: {error:#}"), cx);
            }
        }
        if (self.plugins.preview.take_refresh_pending() || prepared) && !self.plugins.chain.has_pending_restarts() {
            self.refresh_plugin_preview(cx);
        }
        if let Some(message) = self.plugins.preview.failure() {
            self.turn_plugin_preview_off(message, cx);
        } else if self.plugins.preview.is_attached() {
            let tail_frames = self.plugins.chain.preview_tail_frames(ui);
            if self.plugins.preview.tail_changed(tail_frames)
                && let Some(player) = &self.player
            {
                player.set_insert_tail(tail_frames);
            }
        }
        if let Some(player) = &mut self.player {
            drop(player.take_retired_inserts());
        }
        prepared
    }

    /// Turns the preview off, saving the choice like the user turning it off, and shows why.
    fn turn_plugin_preview_off(&mut self, message: String, cx: &mut Context<Self>) {
        let mut settings = self.settings.clone();
        settings.preview_plugin_chain = false;
        self.apply_settings(settings, cx);
        self.report_status(message, cx);
    }
}

#[cfg(test)]
mod test_preview_source {
    use std::io::Write as _;

    use super::*;

    #[test]
    fn same_path_retry_invalidates_old_tag_generation() {
        let file = tempfile::NamedTempFile::new().expect("temporary source");
        let mut source = PreviewSource::default();
        source.begin(file.path());
        let (version, generation) = source.token().expect("captured source");
        assert!(source.accepts(&version, generation));
        source.begin(file.path());
        assert!(!source.accepts(&version, generation));
        assert!(!source.needs_reload(file.path(), Some(file.path())));
        source.clear();
        assert!(source.needs_reload(file.path(), Some(file.path())));
        assert!(source.token().is_none());
    }

    #[test]
    fn changed_source_requires_new_decoder_and_rejects_old_analysis() {
        let mut file = tempfile::NamedTempFile::new().expect("temporary source");
        let mut source = PreviewSource::default();
        source.begin(file.path());
        let (version, generation) = source.token().expect("captured source");
        let mut record = AnalysisRecord {
            path: file.path().to_path_buf(),
            file_size: version.identity.0,
            modified_time_ms: version.identity.1,
            status: audiobatch_core::AnalysisStatus::Analyzed,
            ..AnalysisRecord::default()
        };
        assert!(source.matches_record(&record));
        file.write_all(b"replacement bytes").expect("replace source");
        assert!(source.needs_reload(file.path(), Some(file.path())));
        assert!(!source.accepts(&version, generation));
        assert!(!source.matches_record(&record));
        source.begin(file.path());
        assert!(!source.matches_record(&record));
        let (file_size, modified_time_ms) = file_identity(file.path()).expect("replacement identity");
        record.file_size = file_size;
        record.modified_time_ms = modified_time_ms;
        record.status = audiobatch_core::AnalysisStatus::Cached;
        assert!(source.matches_record(&record));
    }

    #[test]
    fn released_or_missing_decoder_is_retryable() {
        let file = tempfile::NamedTempFile::new().expect("temporary source");
        let mut source = PreviewSource::default();
        source.begin(file.path());
        assert!(source.needs_reload(file.path(), None));
        assert!(source.needs_reload(Path::new("another.wav"), Some(file.path())));
        source.begin(Path::new("/missing/audiobatch/preview.wav"));
        assert!(source.token().is_none());
        assert!(source.needs_reload(file.path(), Some(file.path())));
    }
}
