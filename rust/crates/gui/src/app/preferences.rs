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
    AnyWindowHandle, AppContext as _, Bounds, Context, Entity, Subscription, TitlebarOptions, WeakEntity, Window,
    WindowBounds, WindowOptions, px, size,
};

use super::AppView;
use crate::settings::{GuiSettings, PreviewViewMode};
use crate::settings_window::{
    SETTINGS_HEIGHT, SETTINGS_TITLE, SETTINGS_WIDTH, SettingsEvent, SettingsSnapshot, SettingsView,
};
use crate::waveform::CueMarker;

/// Smallest size of the settings window.
/// The width keeps the loudness graph checkbox and both range fields on one row without squeezing the checkbox label.
const SETTINGS_MINIMUM_SIZE: (f32, f32) = (520.0, 260.0);

/// The open settings window and the subscription to its changes.
pub(super) struct SettingsWindow {
    handle: AnyWindowHandle,
    view: WeakEntity<SettingsView>,
    _subscription: Subscription,
}

impl AppView {
    /// Opens settings on the main window's display, or brings the existing settings window to the front.
    /// Session dimensions are clamped to the target display's visible bounds when centered.
    pub(super) fn open_settings_window(&mut self, window: &Window, cx: &mut Context<Self>) {
        if let Some(settings_window) = &self.settings_window
            && settings_window
                .handle
                .update(cx, |_, window, _| window.activate_window())
                .is_ok()
        {
            return;
        }
        let snapshot = self.settings_snapshot();
        let display_id = window.display(cx).map(|display| display.id());
        let dimensions = self
            .settings_window_size
            .unwrap_or(size(px(SETTINGS_WIDTH), px(SETTINGS_HEIGHT)));
        let bounds = Bounds::centered(display_id, dimensions, cx);
        let options = WindowOptions {
            display_id,
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

    /// Turns colouring the waveform from the Serato Overview tag on or off, from the Options menu.
    pub(super) fn toggle_serato_colours(&mut self, cx: &mut Context<Self>) {
        let mut settings = self.settings.clone();
        settings.serato_waveform_colours = !settings.serato_waveform_colours;
        self.apply_settings(settings, cx);
    }

    /// Selects the source visualization and arrangement, from a menu or the preview.
    pub(super) fn set_preview_view_mode(&mut self, mode: PreviewViewMode, cx: &mut Context<Self>) {
        let mut settings = self.settings.clone();
        settings.preview_view_mode = mode;
        self.apply_settings(settings, cx);
    }

    /// Shows or hides source sample-peak overload indicators without changing true peaks or lines.
    pub(super) fn toggle_sample_overloads(&mut self, cx: &mut Context<Self>) {
        let mut settings = self.settings.clone();
        settings.show_sample_overloads = !settings.show_sample_overloads;
        self.apply_settings(settings, cx);
    }

    /// Shows or hides source true-peak overload indicators without changing sample peaks or lines.
    pub(super) fn toggle_true_peak_overloads(&mut self, cx: &mut Context<Self>) {
        let mut settings = self.settings.clone();
        settings.show_true_peak_overloads = !settings.show_true_peak_overloads;
        self.apply_settings(settings, cx);
    }

    /// Toggles vertical overload lines, retaining the preference even while both indicator types are hidden.
    pub(super) fn toggle_overload_lines(&mut self, cx: &mut Context<Self>) {
        let mut settings = self.settings.clone();
        settings.show_overload_lines = !settings.show_overload_lines;
        self.apply_settings(settings, cx);
    }

    /// Shows or hides the album artwork beside the file name, from the Options menu.
    pub(super) fn toggle_album_artwork(&mut self, cx: &mut Context<Self>) {
        let mut settings = self.settings.clone();
        settings.show_album_artwork = !settings.show_album_artwork;
        self.apply_settings(settings, cx);
    }

    /// Collapses the preview into its compact row, or expands it again with the divider where it was.
    /// The choice lasts for this session only.
    pub(super) fn toggle_preview_section(&mut self, cx: &mut Context<Self>) {
        self.preview_compact = !self.preview_compact;
        self.menu_state.preview_compact = self.preview_compact;
        self.refresh_menus(cx);
        cx.notify();
    }

    /// Shows or hides the loudness graph, from the Options menu.
    pub(super) fn toggle_loudness_graph(&mut self, cx: &mut Context<Self>) {
        let mut settings = self.settings.clone();
        settings.show_loudness_graph = !settings.show_loudness_graph;
        self.apply_settings(settings, cx);
    }

    /// Turns playing the preview through the plugin chain on or off,
    /// from the Options menu or the Preview chain option of the wide controls.
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

    /// Supplies matching cue metadata to the current load's deferred play or seek intent.
    /// Resolution waits for the source duration too, and Stop or an explicit seek discards the intent.
    /// The earliest nonnegative finite cue is used only within the first half of the file, otherwise the start is zero.
    pub(super) fn start_at_first_cue(&mut self, path: &Path, cues: &[CueMarker], _cx: &Context<Self>) {
        if !self.settings.play_from_first_cue_point {
            return;
        }
        if let Some(intent) = &mut self.start_after_cues {
            intent.receive_cues(path, cues.iter().map(|cue| cue.seconds));
            self.resolve_cue_start();
        }
    }

    /// Resolves a pending cue request exactly once after matching metadata and this load's duration are ready.
    /// Paused selections only seek, while a play intent also starts playback.
    pub(super) fn resolve_cue_start(&mut self) {
        let Some(player) = &mut self.player else {
            return;
        };
        let start = self
            .start_after_cues
            .as_ref()
            .and_then(|intent| intent.position(player.loaded_path(), player.duration_seconds()));
        if let Some(start) = start {
            let play = self.start_after_cues.take().is_some_and(|intent| intent.should_play());
            player.seek(start);
            if play {
                player.play();
            }
        }
    }

    /// Applies changed settings: reopens the player for a changed output configuration,
    /// gives the preview a changed plugin chain option, updates the waveform and the menu, saves them,
    /// and shows the result in an open settings window.
    /// An output configuration that cannot be opened keeps the previous one.
    /// A changed compensation builds a new chain, so it takes effect at once instead of at the next start or seek.
    pub(super) fn apply_settings(&mut self, settings: GuiSettings, cx: &mut Context<Self>) {
        let mut settings = settings;
        if !settings.play_from_first_cue_point
            && self.start_after_cues.take().is_some_and(|intent| intent.should_play())
            && let Some(player) = &mut self.player
        {
            player.play();
        }
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
        let (show, minimum, maximum, serato_colours) = (
            settings.show_loudness_graph,
            settings.loudness_minimum,
            settings.loudness_maximum,
            settings.serato_waveform_colours,
        );
        self.waveform.update(cx, |waveform, cx| {
            waveform.set_show_loudness(show, cx);
            waveform.set_loudness_range(minimum, maximum, cx);
            waveform.set_serato_colours(serato_colours, cx);
            waveform.set_view_mode(settings.preview_view_mode, cx);
            waveform.set_overload_settings(
                settings.show_sample_overloads,
                settings.show_true_peak_overloads,
                settings.show_overload_lines,
                cx,
            );
        });
        self.menu_state.preview_view_mode = settings.preview_view_mode;
        self.menu_state.show_sample_overloads = settings.show_sample_overloads;
        self.menu_state.show_true_peak_overloads = settings.show_true_peak_overloads;
        self.menu_state.show_overload_lines = settings.show_overload_lines;
        self.menu_state.show_loudness_graph = show;
        self.menu_state.serato_colours = serato_colours;
        self.menu_state.show_album_artwork = settings.show_album_artwork;
        self.menu_state.preview_plugin_chain = settings.preview_plugin_chain;
        self.menu_state.compensate_preview_latency = settings.compensate_preview_latency;
        self.refresh_menus(cx);
        let artwork_changed = settings.show_album_artwork != self.settings.show_album_artwork;
        self.settings = settings;
        self.save_settings();
        if preview_changed {
            self.refresh_plugin_preview(cx);
        }
        if artwork_changed {
            self.refresh_artwork(cx);
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
            devices: if cfg!(test) {
                Vec::new()
            } else {
                Player::output_device_names()
            },
            capabilities: if cfg!(test) {
                None
            } else {
                device_capabilities(device.as_deref()).ok()
            },
            device,
            sample_rate: self.player.as_ref().map(Player::output_rate),
            buffer_size: self.player.as_ref().and_then(Player::buffer_size),
            settings: self.settings.clone(),
        }
    }

    /// Applies the settings the user changed in the settings window.
    /// Only fields changed from its displayed snapshot are merged, preserving newer choices from other surfaces.
    fn on_settings_event(&mut self, _view: Entity<SettingsView>, event: &SettingsEvent, cx: &mut Context<Self>) {
        match event {
            SettingsEvent::Changed { previous, settings } => {
                let settings = merge_settings_window(&self.settings, previous, settings);
                self.apply_settings(settings, cx);
            }
            SettingsEvent::Resized(dimensions) => self.settings_window_size = Some(*dimensions),
        }
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
        self.start_after_cues = None;
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
                Err(error.into())
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
    Ok(Player::with_config(output)
        .or_else(|_| {
            Player::with_config(&OutputConfig {
                device: output.device.clone(),
                ..OutputConfig::default()
            })
        })
        .or_else(|_| Player::new(None))?)
}

/// Applies only the editable fields changed from the displayed snapshot to the canonical settings.
/// A stale edit cannot restore another control's older value, including controls shared with the Options menu.
fn merge_settings_window(current: &GuiSettings, previous: &GuiSettings, changed: &GuiSettings) -> GuiSettings {
    let mut merged = current.clone();
    macro_rules! merge_fields {
        ($($field:ident),+ $(,)?) => {
            $(if changed.$field != previous.$field {
                merged.$field = changed.$field;
            })+
        };
    }
    if changed.output_device != previous.output_device {
        merged.output_device.clone_from(&changed.output_device);
    }
    merge_fields!(
        sample_rate,
        buffer_size,
        preview_plugin_chain,
        compensate_preview_latency,
        continue_playback,
        play_from_first_cue_point,
        preview_view_mode,
        show_sample_overloads,
        show_true_peak_overloads,
        show_overload_lines,
        serato_waveform_colours,
        show_loudness_graph,
        show_gain_adjusted_metrics,
        loudness_minimum,
        loudness_maximum,
    );
    if changed.output_device != previous.output_device {
        merged.sample_rate = changed.sample_rate;
        merged.buffer_size = changed.buffer_size;
    }
    merged
}

#[cfg(test)]
mod test_settings_merge {
    use super::*;

    #[test]
    fn audio_settings_preserve_both_serato_colour_toggle_states() {
        for enabled in [false, true] {
            let current = GuiSettings {
                serato_waveform_colours: enabled,
                ..GuiSettings::default()
            };
            let stale = GuiSettings {
                serato_waveform_colours: !enabled,
                ..GuiSettings::default()
            };
            let merged = merge_settings_window(
                &current,
                &stale,
                &GuiSettings {
                    buffer_size: Some(512),
                    ..stale.clone()
                },
            );
            assert_eq!(merged.serato_waveform_colours, enabled);
            assert_eq!(merged.buffer_size, Some(512));
        }
    }

    #[test]
    fn gain_metric_preference_merges_edits_without_restoring_stale_values() {
        for enabled in [false, true] {
            let previous = GuiSettings {
                show_gain_adjusted_metrics: !enabled,
                ..GuiSettings::default()
            };
            let current = GuiSettings {
                show_gain_adjusted_metrics: enabled,
                ..previous.clone()
            };
            let unrelated = GuiSettings {
                buffer_size: Some(512),
                ..previous.clone()
            };
            assert_eq!(
                merge_settings_window(&current, &previous, &unrelated).show_gain_adjusted_metrics,
                enabled
            );
            assert_eq!(
                merge_settings_window(&previous, &previous, &current).show_gain_adjusted_metrics,
                enabled
            );
        }
    }

    #[test]
    fn audio_settings_preserve_newer_column_preferences() {
        let current = GuiSettings {
            hidden_columns: Some(vec![String::from("path")]),
            ..GuiSettings::default()
        };
        let stale = GuiSettings {
            hidden_columns: Some(vec![String::from("peak_left")]),
            ..GuiSettings::default()
        };
        let merged = merge_settings_window(
            &current,
            &stale,
            &GuiSettings {
                buffer_size: Some(512),
                ..stale.clone()
            },
        );
        assert_eq!(merged.hidden_columns, current.hidden_columns);
        assert_eq!(merged.buffer_size, Some(512));
    }

    #[test]
    fn audio_settings_preserve_both_artwork_toggle_states() {
        for enabled in [false, true] {
            let current = GuiSettings {
                show_album_artwork: enabled,
                ..GuiSettings::default()
            };
            let stale = GuiSettings {
                show_album_artwork: !enabled,
                ..GuiSettings::default()
            };
            let merged = merge_settings_window(
                &current,
                &stale,
                &GuiSettings {
                    buffer_size: Some(512),
                    ..stale.clone()
                },
            );
            assert_eq!(merged.show_album_artwork, enabled);
            assert_eq!(merged.buffer_size, Some(512));
        }
    }

    #[test]
    fn audio_settings_keep_newer_normalization_targets() {
        let current = GuiSettings {
            normalize_target_lufs: -12.0,
            normalize_mode: crate::settings::NormalizeMode::Loudness,
            ..GuiSettings::default()
        };
        let merged = merge_settings_window(&current, &GuiSettings::default(), &GuiSettings::default());
        assert!((merged.normalize_target_lufs + 12.0).abs() < f64::EPSILON);
        assert_eq!(merged.normalize_mode, current.normalize_mode);
    }

    #[test]
    fn stale_device_edits_preserve_every_preview_mode_and_overload_combination() {
        for mode in PreviewViewMode::ALL {
            for sample in [false, true] {
                for true_peak in [false, true] {
                    for lines in [false, true] {
                        let current = GuiSettings {
                            preview_view_mode: mode,
                            show_sample_overloads: sample,
                            show_true_peak_overloads: true_peak,
                            show_overload_lines: lines,
                            ..GuiSettings::default()
                        };
                        let previous = GuiSettings {
                            show_sample_overloads: !sample,
                            show_true_peak_overloads: !true_peak,
                            show_overload_lines: !lines,
                            ..GuiSettings::default()
                        };
                        let changed = GuiSettings {
                            buffer_size: Some(512),
                            ..previous.clone()
                        };
                        let merged = merge_settings_window(&current, &previous, &changed);
                        assert_eq!(merged.preview_view_mode, mode);
                        assert_eq!(merged.show_sample_overloads, sample);
                        assert_eq!(merged.show_true_peak_overloads, true_peak);
                        assert_eq!(merged.show_overload_lines, lines);
                        assert_eq!(merged.buffer_size, Some(512));
                    }
                }
            }
        }
    }

    #[test]
    fn preview_edits_apply_only_the_requested_field_from_a_stale_snapshot() {
        let previous = GuiSettings::default();
        let current = GuiSettings {
            preview_view_mode: PreviewViewMode::Spectrogram,
            show_sample_overloads: false,
            show_true_peak_overloads: false,
            show_overload_lines: false,
            serato_waveform_colours: false,
            ..previous.clone()
        };
        let edits: [fn(&mut GuiSettings); 5] = [
            |settings| settings.preview_view_mode = PreviewViewMode::Stacked,
            |settings| settings.show_sample_overloads = false,
            |settings| settings.show_true_peak_overloads = false,
            |settings| settings.show_overload_lines = false,
            |settings| settings.serato_waveform_colours = false,
        ];
        for edit in edits {
            let mut changed = previous.clone();
            edit(&mut changed);
            let mut expected = current.clone();
            edit(&mut expected);
            assert_eq!(merge_settings_window(&current, &previous, &changed), expected);
        }
        let toggles: [fn(&mut GuiSettings, bool); 4] = [
            |settings, enabled| settings.show_sample_overloads = enabled,
            |settings, enabled| settings.show_true_peak_overloads = enabled,
            |settings, enabled| settings.show_overload_lines = enabled,
            |settings, enabled| settings.serato_waveform_colours = enabled,
        ];
        for toggle in toggles {
            for enabled in [false, true] {
                let mut current = current.clone();
                toggle(&mut current, !enabled);
                let mut previous = previous.clone();
                toggle(&mut previous, !enabled);
                let mut changed = previous.clone();
                toggle(&mut changed, enabled);
                let mut expected = current.clone();
                toggle(&mut expected, enabled);
                assert_eq!(merge_settings_window(&current, &previous, &changed), expected);
            }
        }
    }

    #[test]
    fn selecting_a_device_resets_stale_rate_and_buffer_choices_together() {
        let previous = GuiSettings::default();
        let current = GuiSettings {
            sample_rate: Some(96_000),
            buffer_size: Some(256),
            ..previous.clone()
        };
        let changed = GuiSettings {
            output_device: Some(String::from("New device")),
            ..previous.clone()
        };
        let merged = merge_settings_window(&current, &previous, &changed);
        assert_eq!(merged.output_device, changed.output_device);
        assert_eq!(merged.sample_rate, None);
        assert_eq!(merged.buffer_size, None);
    }

    #[test]
    fn audio_settings_apply_device_changes_without_changing_the_menu_choice() {
        let changed = GuiSettings {
            output_device: Some(String::from("New device")),
            sample_rate: Some(48_000),
            ..GuiSettings::default()
        };
        let merged = merge_settings_window(&GuiSettings::default(), &GuiSettings::default(), &changed);
        assert_eq!(merged.output_device, changed.output_device);
        assert_eq!(merged.sample_rate, changed.sample_rate);
        assert!(merged.show_album_artwork);
    }
}
