/// Constants shared by the AudioBatchComponent implementation files.
/// Holds the main view's file filters, slider ranges, and context menu item ids.
/// GUI geometry is defined separately in GuiLayout.h.

#pragma once

#include <cstdint>

namespace audiobatch
{
/// Identifiers for menu items.
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

/// Identifiers shared by results-table layout, painting, and sorting.
enum AudioFileTableColumn : std::uint8_t {
    columnName = 1,
    columnPath,
    columnType,
    columnBitrate,
    columnSampleRate,
    columnPeakLeft,
    columnPeakRight,
    columnOverallPeak,
    columnOverallTruePeak,
    columnMaxShortTermLufs,
    columnIntegratedLufs,
    columnCustomGain,
    columnStatus,
};

constexpr auto supportedAudioFilePatterns = "*.wav;*.aif;*.aiff;*.flac;*.mp3";

/// Range of the waveform zoom slider in percent of the unzoomed view and its conversion to a scale factor.
namespace zoom_range
{
/// Smallest zoom level in percent, which is also the initial value and shows the whole file.
constexpr int minimumPercent = 100;
/// Largest zoom level in percent.
constexpr int maximumPercent = 2000;
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

}  // namespace audiobatch
