//! Stable element ids of the main-window controls that both GUIs share.
//!
//! The ids match `tests/gui_controls.json` at the repository root and the component ids of the C++ application,
//! so GUI integration tests find the same control in both applications.
//! Changing a value here requires changing the manifest and the C++ `GuiControlIds.h` together.

/// The results table.
pub const RESULTS: &str = "results";
/// The Play/Stop button.
pub const PLAY_STOP: &str = "play-stop";
/// The Process button, which shows Cancel while a run is active.
pub const PROCESS_SELECTED: &str = "process-selected";
/// The chevron that collapses and expands the preview.
pub const PREVIEW_SECTION: &str = "preview-section";
/// The waveform zoom slider.
pub const ZOOM_TRACK: &str = "zoom-track";
/// The per-file input gain slider.
pub const GAIN_TRACK: &str = "gain-track";
/// The option that normalizes before the plugins.
pub const PROCESS_NORMALIZE_FIRST: &str = "process-normalize-first";
/// The option that overwrites existing outputs.
pub const NORMALIZE_OVERWRITE: &str = "normalize-overwrite";
/// The option that plays the preview through the plugin chain.
pub const PREVIEW_CHAIN: &str = "preview-chain";
/// The Peak normalization mode button.
pub const NORMALIZE_PEAK: &str = "normalize-peak";
/// The Loudness normalization mode button.
pub const NORMALIZE_LOUDNESS: &str = "normalize-loudness";
/// The normalization target slider.
pub const NORMALIZE_TARGET: &str = "normalize-target";
/// The Normalize button.
pub const NORMALIZE_SELECTED: &str = "normalize-selected";
/// The Plugin Chain button.
pub const PLUGIN_CHAIN: &str = "plugin-chain";
/// The Audio Settings button.
pub const AUDIO_SETTINGS: &str = "audio-settings";
/// The divider that resizes the controls column.
pub const CONTROLS_WIDTH_DIVIDER: &str = "controls-width-divider";
/// The divider that resizes the information panel.
pub const INFORMATION_WIDTH_DIVIDER: &str = "information-width-divider";
/// The status text of the status bar.
pub const STATUS: &str = "status";

/// Accessible name of the results table.
pub const RESULTS_LABEL: &str = "Results";
/// Accessible name and caption of the zoom slider.
pub const ZOOM_LABEL: &str = "Zoom";
/// Accessible name and caption of the input gain slider.
pub const GAIN_LABEL: &str = "Input gain";
/// Accessible name of the normalization target slider.
pub const NORMALIZE_TARGET_LABEL: &str = "Normalization target";
/// Accessible name of the divider that resizes the controls column.
pub const CONTROLS_WIDTH_DIVIDER_LABEL: &str = "Resize controls";
/// Accessible name of the divider that resizes the information panel.
pub const INFORMATION_WIDTH_DIVIDER_LABEL: &str = "Resize information panel";

/// The root element of the Audio Settings window, a separate window from the button that opens it.
pub const AUDIO_SETTINGS_WINDOW: &str = "audio-settings-window";
/// The root element of the Plugin Chain window, a separate window from the button that opens it.
pub const PLUGIN_CHAIN_WINDOW: &str = "plugin-chain-window";
