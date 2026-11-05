/// Stable component ids and accessibility titles of the main-window controls.
/// The ids and titles match `tests/gui_controls.json` at the repository root,
/// which the Rust application shares, so GUI integration tests can find the same control in both applications.
/// Changing a value here requires changing the manifest and the Rust element ids together.

#pragma once

namespace audiobatch::control_ids
{
constexpr auto RESULTS {"results"};
constexpr auto PLAY_STOP {"play-stop"};
constexpr auto PROCESS_SELECTED {"process-selected"};
constexpr auto PREVIEW_SECTION {"preview-section"};
constexpr auto ZOOM_TRACK {"zoom-track"};
constexpr auto GAIN_TRACK {"gain-track"};
constexpr auto PROCESS_NORMALIZE_FIRST {"process-normalize-first"};
constexpr auto NORMALIZE_OVERWRITE {"normalize-overwrite"};
constexpr auto PREVIEW_CHAIN {"preview-chain"};
constexpr auto NORMALIZE_PEAK {"normalize-peak"};
constexpr auto NORMALIZE_LOUDNESS {"normalize-loudness"};
constexpr auto NORMALIZE_TARGET {"normalize-target"};
constexpr auto NORMALIZE_SELECTED {"normalize-selected"};
constexpr auto PLUGIN_CHAIN {"plugin-chain"};
constexpr auto AUDIO_SETTINGS {"audio-settings"};
constexpr auto CONTROLS_WIDTH_DIVIDER {"controls-width-divider"};
constexpr auto INFORMATION_WIDTH_DIVIDER {"information-width-divider"};
constexpr auto STATUS {"status"};
}  // namespace audiobatch::control_ids

namespace audiobatch::control_titles
{
constexpr auto RESULTS {"Results"};
constexpr auto ZOOM_TRACK {"Zoom"};
constexpr auto GAIN_TRACK {"Input gain"};
constexpr auto NORMALIZE_TARGET {"Normalization target"};
constexpr auto PLUGIN_CHAIN {"Plugin Chain"};
constexpr auto CONTROLS_WIDTH_DIVIDER {"Resize controls"};
constexpr auto INFORMATION_WIDTH_DIVIDER {"Resize information panel"};
}  // namespace audiobatch::control_titles
