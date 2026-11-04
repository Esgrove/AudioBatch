//! Debounced selected-file gain and clipping predictions with one cooperatively cancelled worker.
//! Results remain transient presentation state and never enter source records or the shared cache.

use std::path::PathBuf;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};

use audiobatch_core::gain_loudness::{GainLoudness, measure_gained_loudness};
use audiobatch_core::output_file::SourceIdentity;
use audiobatch_core::record::file_identity;
use gpui_kit::Context;

use super::AppView;

/// Waits for a gain gesture to settle before starting another full-file read.
const DEBOUNCE: Duration = Duration::from_millis(350);

/// Captures source and gain authority without cloning waveform or spectral data.
#[derive(Clone, Debug, PartialEq, Eq)]
struct PredictionKey {
    path: PathBuf,
    file_size: u64,
    modified_time_ms: i64,
    source: Option<SourceIdentity>,
    generation: u64,
    gain_bits: u32,
}

/// Owns a single admitted reader, its latest desired replacement, and one completed result or failure.
#[derive(Default)]
pub(super) struct GainPrediction {
    desired: Option<PredictionKey>,
    changed_at: Option<Instant>,
    active: Option<Arc<AtomicBool>>,
    completed: Option<(PredictionKey, Option<GainLoudness>)>,
    pub(super) stopped: bool,
}

impl GainPrediction {
    /// Retains shared decoder admission until the worker has actually returned.
    pub(super) const fn has_active_reader(&self) -> bool {
        self.active.is_some()
    }

    /// Invalidates publication and requests worker exit without admitting a replacement before it drains.
    pub(super) fn cancel(&mut self) {
        if let Some(cancel) = &self.active {
            cancel.store(true, Ordering::Release);
        }
        self.desired = None;
        self.completed = None;
        self.changed_at = None;
    }
}

impl Drop for GainPrediction {
    fn drop(&mut self) {
        self.cancel();
    }
}

impl AppView {
    /// Returns only a result matching the current selection, gain, option, and source generation.
    /// This check also hides old values immediately, before the next worker-admission tick.
    pub(super) fn gained_loudness(&self, cx: &gpui_kit::App) -> Option<GainLoudness> {
        let key = self.gain_prediction_key(cx)?;
        self.gain_prediction
            .completed
            .as_ref()
            .filter(|(completed, _)| *completed == key)?
            .1
    }

    /// Coalesces changes and drains cancelled work before admitting one replacement.
    /// Called by the existing application poll, never from rendering.
    pub(super) fn update_gain_prediction(&mut self, cx: &mut Context<Self>) {
        let desired = self.gain_prediction_key(cx);
        if desired != self.gain_prediction.desired {
            self.gain_prediction.cancel();
            self.gain_prediction.desired = desired;
            self.gain_prediction.changed_at = Some(Instant::now());
            cx.notify();
        }
        if self.gain_prediction.active.is_some()
            || self.enrichment.has_active_reader()
            || self.gain_prediction.completed.is_some()
            || self
                .gain_prediction
                .changed_at
                .is_none_or(|changed| changed.elapsed() < DEBOUNCE)
        {
            return;
        }
        let Some(key) = self.gain_prediction.desired.clone() else {
            return;
        };
        let cancel = Arc::new(AtomicBool::new(false));
        self.gain_prediction.active = Some(Arc::clone(&cancel));
        let worker_key = key.clone();
        let publication_cancel = Arc::clone(&cancel);
        let worker = cx.background_executor().spawn(async move {
            let cancelled = || cancel.load(Ordering::Acquire);
            let current = || {
                file_identity(&worker_key.path) == Some((worker_key.file_size, worker_key.modified_time_ms))
                    && worker_key
                        .source
                        .as_ref()
                        .is_none_or(|source| SourceIdentity::capture(&worker_key.path).ok().as_ref() == Some(source))
            };
            if cancelled() || !current() {
                return None;
            }
            let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                measure_gained_loudness(&worker_key.path, f32::from_bits(worker_key.gain_bits), &cancelled)
            }));
            match result {
                Ok(Ok(value)) if !cancelled() && current() => Some(value),
                Ok(Err(error)) => {
                    if !cancelled() {
                        tracing::debug!(path = %worker_key.path.display(), %error, "Loudness prediction unavailable");
                    }
                    None
                }
                _ => None,
            }
        });
        // The foreground drain is retained even after cancellation, preventing overlapping decoder jobs.
        self.gain_prediction_task = Some(cx.spawn(async move |this, cx| {
            let result = worker.await;
            let _ = this.update(cx, |view, cx| {
                view.gain_prediction.active = None;
                if !publication_cancel.load(Ordering::Acquire)
                    && view.gain_prediction.desired.as_ref() == Some(&key)
                    && view.gain_prediction_key(cx).as_ref() == Some(&key)
                {
                    // A failed key is remembered, so polling cannot retry a broken source forever.
                    view.gain_prediction.completed = Some((key, result));
                }
                view.start_enrichment(cx);
                view.update_gain_prediction(cx);
                cx.notify();
            });
        }));
    }

    /// Derives the selected presentation request without filesystem work on the rendering thread.
    fn gain_prediction_key(&self, cx: &gpui_kit::App) -> Option<PredictionKey> {
        if self.gain_prediction.stopped
            || !self.settings.show_gain_adjusted_metrics
            || self.is_demo()
            || self.analysis_workers > 0
            || self.is_running()
        {
            return None;
        }
        let record = self.selected_record(cx)?;
        if self.artwork_rewrites.contains(&record.path) {
            return None;
        }
        let gain = record.custom_gain_db.filter(|gain| gain.is_finite())?;
        Some(PredictionKey {
            path: record.path.clone(),
            file_size: record.file_size,
            modified_time_ms: record.modified_time_ms,
            source: record.source_identity.clone(),
            generation: self.preview_source.token().map_or(0, |(_, generation)| generation),
            gain_bits: gain.to_bits(),
        })
    }
}

#[cfg(test)]
mod test_prediction_cleanup {
    use super::*;

    /// Makes a key without accessing real application storage.
    fn key(gain: f32) -> PredictionKey {
        PredictionKey {
            path: PathBuf::from("source.wav"),
            file_size: 1,
            modified_time_ms: 2,
            source: None,
            generation: 3,
            gain_bits: gain.to_bits(),
        }
    }

    #[test]
    fn cancellation_retains_admission_until_worker_drains_and_clears_failed_attempt() {
        let cancel = Arc::new(AtomicBool::new(false));
        let mut state = GainPrediction {
            desired: Some(key(6.0)),
            active: Some(Arc::clone(&cancel)),
            completed: Some((key(6.0), None)),
            changed_at: Some(Instant::now()),
            stopped: false,
        };
        state.cancel();
        assert!(cancel.load(Ordering::Acquire));
        assert!(state.has_active_reader());
        assert!(state.desired.is_none());
        assert!(state.completed.is_none());
        assert!(state.changed_at.is_none());
    }

    #[test]
    fn view_drop_requests_exit_for_owned_worker() {
        let cancel = Arc::new(AtomicBool::new(false));
        let state = GainPrediction {
            active: Some(Arc::clone(&cancel)),
            desired: None,
            changed_at: None,
            completed: None,
            stopped: false,
        };
        drop(state);
        assert!(cancel.load(Ordering::Acquire));
    }

    #[test]
    fn source_gain_and_generation_are_all_publication_authority() {
        let original = key(6.0);
        assert_ne!(original, key(12.0));
        let mut changed = original.clone();
        changed.generation += 1;
        assert_ne!(original, changed);
        changed = original.clone();
        changed.file_size += 1;
        assert_ne!(original, changed);
        changed = original.clone();
        changed.path = PathBuf::from("other.wav");
        assert_ne!(original, changed);
    }
}
