//! The preferences of the main view: the Audio Settings window, the output device, and the preview options.
//!
//! Every way of changing a setting ends in `apply_settings`:
//! the settings window and the Options menu.
//! It reopens the player when the output configuration changed, keeps the plugin chain preview in step,
//! updates the waveform and the menu, saves the settings,
//! and syncs an open settings window with what was actually applied.

use std::path::{Path, PathBuf};

use audiobatch_playback::{OutputConfig, Player, device_capabilities};
use gpui_kit::{
    AnyWindowHandle, AppContext as _, Bounds, Context, Entity, Subscription, TitlebarOptions, WeakEntity, WindowBounds,
    WindowOptions, px, size,
};

use super::AppView;
use crate::menus::set_menus;
use crate::results::ResultRow;
use crate::settings::GuiSettings;
use crate::settings_window::{
    SETTINGS_HEIGHT, SETTINGS_TITLE, SETTINGS_WIDTH, SettingsEvent, SettingsSnapshot, SettingsView,
};
use crate::waveform::CueMarker;

/// Smallest size of the settings window.
/// The width keeps the loudness graph checkbox and both range fields on one row without squeezing the checkbox label.
const SETTINGS_MINIMUM_SIZE: (f32, f32) = (520.0, 320.0);

/// Playhead position in seconds below which a file counts as not moved yet, so it may jump to its first cue point.
const CUE_START_WINDOW_SECONDS: f64 = 0.5;

/// The open settings window and the subscription to its changes.
pub(super) struct SettingsWindow {
    handle: AnyWindowHandle,
    view: WeakEntity<SettingsView>,
    _subscription: Subscription,
}

impl AppView {
    /// Opens the settings window, or brings it to the front when it is already open.
    pub(super) fn open_settings_window(&mut self, cx: &mut Context<Self>) {
        if let Some(settings_window) = &self.settings_window
            && settings_window
                .handle
                .update(cx, |_, window, _| window.activate_window())
                .is_ok()
        {
            return;
        }
        let snapshot = self.settings_snapshot();
        let bounds = Bounds::centered(None, size(px(SETTINGS_WIDTH), px(SETTINGS_HEIGHT)), cx);
        let options = WindowOptions {
            window_bounds: Some(WindowBounds::Windowed(bounds)),
            window_min_size: Some(size(px(SETTINGS_MINIMUM_SIZE.0), px(SETTINGS_MINIMUM_SIZE.1))),
            titlebar: Some(TitlebarOptions {
                title: Some(SETTINGS_TITLE.into()),
                ..Default::default()
            }),
            focus: true,
            ..Default::default()
        };
        match gpui_kit::open_window(options, cx, move |window, cx| {
            cx.new(|cx| SettingsView::new(&snapshot, window, cx))
        }) {
            Ok((handle, view)) => {
                let subscription = cx.subscribe(&view, Self::on_settings_event);
                self.settings_window = Some(SettingsWindow {
                    handle,
                    view: view.downgrade(),
                    _subscription: subscription,
                });
            }
            Err(error) => {
                self.status = format!("Could not open the settings: {error:#}").into();
                cx.notify();
            }
        }
    }

    /// Shows or hides the loudness graph, from the Options menu.
    pub(super) fn toggle_loudness_graph(&mut self, cx: &mut Context<Self>) {
        let mut settings = self.settings.clone();
        settings.show_loudness_graph = !settings.show_loudness_graph;
        self.apply_settings(settings, cx);
    }

    /// Turns playing the preview through the plugin chain on or off, from the Options menu.
    pub(super) fn toggle_plugin_preview(&mut self, cx: &mut Context<Self>) {
        let mut settings = self.settings.clone();
        settings.preview_plugin_chain = !settings.preview_plugin_chain;
        self.apply_settings(settings, cx);
    }

    /// Turns latency compensation of the plugin chain preview on or off, from the Options menu.
    pub(super) fn toggle_preview_latency_compensation(&mut self, cx: &mut Context<Self>) {
        let mut settings = self.settings.clone();
        settings.compensate_preview_latency = !settings.compensate_preview_latency;
        self.apply_settings(settings, cx);
    }

    /// Switches playback to another output device with its default sample rate and buffer size, from the menu.
    pub(super) fn select_output_device(&mut self, name: &str, cx: &mut Context<Self>) {
        let mut settings = self.settings.clone();
        settings.output_device = Some(name.to_string());
        settings.sample_rate = None;
        settings.buffer_size = None;
        self.apply_settings(settings, cx);
    }

    /// Starts the selected file at its first cue point when the option is on,
    /// as long as the playhead has not moved since the file was loaded.
    pub(super) fn start_at_first_cue(&mut self, path: &Path, cues: &[CueMarker], cx: &Context<Self>) {
        if !self.settings.play_from_first_cue_point {
            return;
        }
        let duration = self
            .table
            .read(cx)
            .delegate()
            .row_for(path)
            .and_then(ResultRow::ready_record)
            .map(|record| record.duration_seconds)
            .or_else(|| self.player.as_ref().and_then(Player::duration_seconds));
        let Some(start) = duration.and_then(|duration| first_cue_start(cues, duration)) else {
            return;
        };
        if let Some(player) = &mut self.player
            && player.loaded_path() == Some(path)
            && player.position_seconds() < CUE_START_WINDOW_SECONDS
        {
            player.seek(start);
        }
    }

    /// Applies changed settings: reopens the player for a changed output configuration,
    /// gives the preview a changed plugin chain option, updates the waveform and the menu, saves them,
    /// and shows the result in an open settings window.
    /// An output configuration that cannot be opened keeps the previous one.
    /// A changed compensation builds a new chain, so it takes effect at once instead of at the next start or seek.
    pub(super) fn apply_settings(&mut self, settings: GuiSettings, cx: &mut Context<Self>) {
        let mut settings = settings;
        let preview_changed = settings.preview_plugin_chain != self.settings.preview_plugin_chain
            || settings.compensate_preview_latency != self.settings.compensate_preview_latency;
        let output = settings.output_config();
        if output != self.settings.output_config()
            && let Err(error) = self.reopen_player(&output, cx)
        {
            self.status = format!("Could not open the output device: {error:#}").into();
            settings.output_device.clone_from(&self.settings.output_device);
            settings.sample_rate = self.settings.sample_rate;
            settings.buffer_size = self.settings.buffer_size;
        }
        let (show, minimum, maximum) = (
            settings.show_loudness_graph,
            settings.loudness_minimum,
            settings.loudness_maximum,
        );
        self.waveform.update(cx, |waveform, cx| {
            waveform.set_show_loudness(show, cx);
            waveform.set_loudness_range(minimum, maximum, cx);
        });
        self.menu_state.show_loudness_graph = show;
        self.menu_state.preview_plugin_chain = settings.preview_plugin_chain;
        self.menu_state.compensate_preview_latency = settings.compensate_preview_latency;
        set_menus(cx, &self.menu_state);
        self.settings = settings;
        self.settings.save();
        if preview_changed {
            self.refresh_plugin_preview(cx);
        }
        self.sync_settings_window(cx);
        cx.notify();
    }

    /// Shows the current devices, output configuration, and settings in an open settings window.
    fn sync_settings_window(&self, cx: &mut Context<Self>) {
        let Some(settings_window) = &self.settings_window else {
            return;
        };
        let Some(view) = settings_window.view.upgrade() else {
            return;
        };
        let snapshot = self.settings_snapshot();
        let _ = settings_window.handle.update(cx, |_, window, cx| {
            view.update(cx, |view, cx| view.sync(&snapshot, window, cx));
        });
    }

    /// Collects what the settings window shows from the player and the settings.
    fn settings_snapshot(&self) -> SettingsSnapshot {
        let device = self.player.as_ref().map(|player| player.device_name().to_string());
        SettingsSnapshot {
            devices: Player::output_device_names(),
            capabilities: device_capabilities(device.as_deref()).ok(),
            device,
            sample_rate: self.player.as_ref().map(Player::output_rate),
            buffer_size: self.player.as_ref().and_then(Player::buffer_size),
            settings: self.settings.clone(),
        }
    }

    /// Applies the settings the user changed in the settings window.
    /// The window does not edit the normalization settings,
    /// and its copy of them is stale once the main window changes the mode or a target.
    fn on_settings_event(&mut self, _view: Entity<SettingsView>, event: &SettingsEvent, cx: &mut Context<Self>) {
        let SettingsEvent::Changed(changed) = event;
        let settings = GuiSettings {
            normalize_mode: self.settings.normalize_mode,
            normalize_target_peak_dbfs: self.settings.normalize_target_peak_dbfs,
            normalize_target_lufs: self.settings.normalize_target_lufs,
            ..changed.clone()
        };
        self.apply_settings(settings, cx);
    }

    /// Opens the player with another output configuration, keeping the loaded file, its position, and the gain.
    /// When the configuration cannot be opened, the previous one is opened again.
    ///
    /// # Errors
    /// Returns the error of the configuration that could not be opened.
    fn reopen_player(&mut self, output: &OutputConfig, cx: &mut Context<Self>) -> anyhow::Result<()> {
        let resume = self.player.as_ref().and_then(|player| {
            player
                .loaded_path()
                .map(|path| (path.to_path_buf(), player.position_seconds()))
        });
        let gain = self.player.as_ref().map_or(1.0, Player::insert_gain);
        self.player = None;
        self.plugins.preview.player_replaced();
        let result = match Player::with_config(output) {
            Ok(player) => {
                self.install_player(player, resume, gain);
                Ok(())
            }
            Err(error) => {
                if let Ok(player) = open_player(&self.settings.output_config()) {
                    self.install_player(player, resume, gain);
                }
                Err(error)
            }
        };
        self.menu_state.output_device = self.player.as_ref().map(|player| player.device_name().to_string());
        self.connect_playhead(cx);
        result
    }

    /// Makes a newly opened player current, restoring the gain and the loaded file at its position.
    fn install_player(&mut self, mut player: Player, resume: Option<(PathBuf, f64)>, gain: f32) {
        player.set_insert_gain(gain);
        if let Some((path, position)) = resume {
            player.load(&path, position, false);
        }
        self.player = Some(player);
    }
}

/// Opens the player for an output configuration.
/// A device that is missing or a sample rate it no longer supports falls back to the defaults,
/// first of the same device and then of the system.
///
/// # Errors
/// Returns an error when no output device can be opened at all.
pub(super) fn open_player(output: &OutputConfig) -> anyhow::Result<Player> {
    Player::with_config(output)
        .or_else(|_| {
            Player::with_config(&OutputConfig {
                device: output.device.clone(),
                ..OutputConfig::default()
            })
        })
        .or_else(|_| Player::new(None))
}

/// Returns where to start a file with cue points:
/// the earliest cue point when it is after the start and at or before halfway through the file.
fn first_cue_start(cues: &[CueMarker], duration: f64) -> Option<f64> {
    if !duration.is_finite() || duration <= 0.0 {
        return None;
    }
    cues.iter()
        .map(|cue| cue.seconds)
        .filter(|seconds| seconds.is_finite() && *seconds >= 0.0)
        .min_by(f64::total_cmp)
        .filter(|&seconds| seconds > 0.0 && seconds <= duration * 0.5)
}

#[cfg(test)]
mod test_first_cue_start {
    use super::*;

    fn cues(seconds: &[f64]) -> Vec<CueMarker> {
        seconds.iter().map(|&seconds| CueMarker { seconds, color: 0 }).collect()
    }

    #[test]
    fn starts_at_the_earliest_cue_in_the_first_half() {
        assert_eq!(first_cue_start(&cues(&[30.0, 12.5, 90.0]), 200.0), Some(12.5));
    }

    #[test]
    fn a_cue_after_halfway_starts_from_zero() {
        assert_eq!(first_cue_start(&cues(&[120.0]), 200.0), None);
        assert_eq!(first_cue_start(&cues(&[100.0]), 200.0), Some(100.0));
    }

    #[test]
    fn no_cues_or_no_duration_start_from_zero() {
        assert_eq!(first_cue_start(&[], 200.0), None);
        assert_eq!(first_cue_start(&cues(&[0.0]), 200.0), None);
        assert_eq!(first_cue_start(&cues(&[10.0]), 0.0), None);
    }
}
