/// Constants shared by the AudioBatchComponent implementation files.
/// Holds the layout sizes, slider ranges, timeouts, and context menu item ids of the main view.
/// Only the AudioBatchComponent*.cpp files include this header.

#pragma once

#include "gui/AudioFileTableModel.h"

#include <cstdint>

namespace audiobatch
{
constexpr auto supportedAudioFilePatterns = "*.wav;*.aif;*.aiff;*.flac;*.mp3";

constexpr int nameColumnMinimumWidth = AudioFileTableModel::minimumColumnWidth(columnName);
constexpr int pathColumnMinimumWidth = AudioFileTableModel::minimumColumnWidth(columnPath);

constexpr int nameColumnDefaultWidth = AudioFileTableModel::initialColumnWidth(columnName);
constexpr int pathColumnDefaultWidth = AudioFileTableModel::initialColumnWidth(columnPath);

/// Pixel sizes, spacing, and timing used to lay out and configure the main view.
namespace layout
{
#if JUCE_WINDOWS
constexpr int titleBarHeight = 30;
constexpr int defaultWindowWidth = 1280;
constexpr int defaultWindowHeight = 720;
#endif

/// Initial width in pixels of the main component before the window is resized by the user.
constexpr int initialComponentWidth = 1024;
/// Initial height in pixels of the main component.
constexpr int initialComponentHeight = 800;

/// Height in pixels of the top bar holding the current folder and status labels.
constexpr int topBarHeight = 34;
/// Width in pixels reserved for the status label at the right end of the top bar.
constexpr int statusLabelWidth = 220;
/// Spacing in pixels between the main sections and between the stacked preview controls.
constexpr int sectionSpacing = 6;
/// Height in pixels of the overall progress bar drawn in the gap above the results table.
constexpr int overallProgressBarHeight = 3;

/// Height in pixels of each row in the results table.
constexpr int resultsTableRowHeight = 24;

/// Minimum height in pixels of the results table in the vertical split.
constexpr double resultsTableMinimumHeight = 240.0;
/// Maximum share of the vertical split for the results table, negative to mark it as a proportion.
constexpr double resultsTableMaximumProportion = -0.85;
/// Preferred share of the vertical split for the results table, negative to mark it as a proportion.
constexpr double resultsTablePreferredProportion = -0.69;
/// Fixed height in pixels of the draggable bar between the results table and the preview area.
constexpr double resizeBarHeight = 6.0;
/// Minimum height in pixels of the waveform preview area in the vertical split.
constexpr double previewMinimumHeight = 180.0;
/// Maximum share of the vertical split for the preview area, negative to mark it as a proportion.
constexpr double previewMaximumProportion = -0.70;
/// Preferred share of the vertical split for the preview area, negative to mark it as a proportion.
constexpr double previewPreferredProportion = -0.31;

/// Height in pixels of the Process, Play, Audio Settings, and Plugins buttons.
constexpr int controlButtonHeight = 28;
/// Width in pixels of the square button that clears the per-file gain.
constexpr int gainClearButtonWidth = 28;
/// Height in pixels of the normalize-before-plugins toggle row.
constexpr int normalizeToggleHeight = 22;
/// Width in pixels of the zoom slider value text box.
constexpr int zoomTextBoxWidth = 50;

/// Width in pixels of the content component in child dialog windows.
constexpr int dialogContentWidth = 500;
/// Height in pixels of the content component in child dialog windows.
constexpr int dialogContentHeight = 150;

constexpr int maxWindowSize = 4096;
}  // namespace layout

/// Range of the waveform zoom slider in percent of the unzoomed view and its conversion to a scale factor.
namespace zoom_range
{
/// Smallest zoom level in percent, which is also the initial value and shows the whole file.
constexpr int minimumPercent = 100;
/// Largest zoom level in percent.
constexpr int maximumPercent = 1500;
/// Converts a zoom percentage into the scale factor the thumbnail expects.
constexpr double percentToScaleFactor = 0.01;
}  // namespace zoom_range

/// Range and step of the per-file gain slider in decibels.
namespace gain_range
{
/// Largest boost or cut in dB the gain slider allows in either direction.
constexpr double maximumDecibels = 12.0;
/// Step size in dB for the slider and for mouse wheel adjustments over the waveform.
constexpr double stepDecibels = 0.1;
}  // namespace gain_range

/// Delay in milliseconds before a tooltip appears when hovering over a control.
constexpr int tooltipDelayMilliseconds = 700;

enum FileMenuItemId : std::uint8_t {
    revealFileMenuItemId = 1,
    openParentDirectoryMenuItemId,
    normalizeMenuItemId,
    normalizeSupportMenuItemId,
    processMenuItemId,
    clearCustomGainMenuItemId,
    moveToTrashMenuItemId,
    removeFromListMenuItemId,
    reanalyzeMenuItemId,
};
}  // namespace audiobatch
