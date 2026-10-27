//! The live preview through the plugin chain in the main view.
//!
//! The player gets a new chain whenever the structure of the chain, the preview options, or the instances change,
//! and none while the preview is off or no plugin is enabled.
//! Before instances are prepared again, the chain is taken out of the player,
//! and the view waits until the audio callback has let go of it, so no processor runs during the reconfiguration.
//! The housekeeping tick passes changed tails on to the player
//! and turns the preview off when a plugin failed, reporting the plugin and its error.

use std::thread;
use std::time::{Duration, Instant};

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

impl AppView {
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
            Err(message) => self.turn_plugin_preview_off(message, cx),
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
    /// A paused preview holds back the edits made in an editor, because edits reach a plugin through processing,
    /// so its chain is taken out first and put back after.
    pub(super) fn capture_plugin_states(&mut self, cx: &mut Context<Self>) {
        let Some(ui) = UiThread::current() else {
            return;
        };
        let paused = !self.player.as_ref().is_some_and(Player::is_playing);
        if self.plugins.preview.is_attached() && paused {
            self.detach_plugin_preview();
            self.plugins.chain.capture_states(&ui);
            self.refresh_plugin_preview(cx);
        } else {
            self.plugins.chain.capture_states(&ui);
        }
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
            if rate_changed {
                self.plugins.chain.set_sample_rate(ui, sample_rate);
            } else {
                self.plugins.chain.prepare_again(ui, &restarts);
            }
        }
        if self.plugins.preview.take_refresh_pending() || prepared {
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
