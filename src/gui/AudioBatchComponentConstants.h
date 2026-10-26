/// Constants shared by the AudioBatchComponent implementation files.
/// Holds the main view's file filters, slider ranges, context menu items, and results table columns,
/// with the conversions from the JUCE integer ids of the menu items and columns.
/// GUI geometry is defined separately in GuiLayout.h.

#pragma once

#include "utils/GuiUtils.h"

#include <cstdint>
#include <optional>

namespace audiobatch
{
/// Items of the per-file context menu, whose values are the JUCE popup menu result ids.
/// New items go at the end, and fileMenuItemFromId names the last one.
enum class FileMenuItemId : std::uint8_t {
    revealFile = 1,
    openParentDirectory,
    normalize,
    normalizeSupport,
    process,
    clearCustomGain,
    moveToTrash,
    removeFromList,
    reanalyze,
};

/// Columns shared by results-table layout, painting, and sorting, whose values are the JUCE table column ids.
/// New columns go at the end, and tableColumnFromId names the last one.
enum class AudioFileTableColumn : std::uint8_t {
    name = 1,
    path,
    type,
    bitrate,
    sampleRate,
    peakLeft,
    peakRight,
    overallPeak,
    overallTruePeak,
    maxShortTermLufs,
    integratedLufs,
    customGain,
    status,
    bitDepth,
};

/// Returns the context menu item for a popup menu result, or nullopt when the menu was dismissed.
[[nodiscard]] constexpr std::optional<FileMenuItemId> fileMenuItemFromId(const int menuResult) noexcept
{
    return utils::enumFromId<FileMenuItemId, FileMenuItemId::revealFile, FileMenuItemId::reanalyze>(menuResult);
}

/// Returns the column for a JUCE table column id, or nullopt for zero, which JUCE uses for no column.
[[nodiscard]] constexpr std::optional<AudioFileTableColumn> tableColumnFromId(const int columnId) noexcept
{
    return utils::enumFromId<AudioFileTableColumn, AudioFileTableColumn::name, AudioFileTableColumn::bitDepth>(
        columnId
    );
}

/// Returns the JUCE table column id of a column, or zero for no column.
[[nodiscard]] constexpr int toColumnId(const std::optional<AudioFileTableColumn> column) noexcept
{
    return column.has_value() ? std::to_underlying(*column) : 0;
}

constexpr auto supportedAudioFilePatterns {"*.wav;*.aif;*.aiff;*.flac;*.mp3"};

/// Range of the waveform zoom slider in percent of the unzoomed view and its conversion to a scale factor.
namespace zoom_range
{
/// Smallest zoom level in percent, which is also the initial value and shows the whole file.
constexpr int minimumPercent {100};
/// Largest zoom level in percent.
constexpr int maximumPercent {2000};
/// Converts a zoom percentage into the scale factor the thumbnail expects.
constexpr double percentToScaleFactor {0.01};
}  // namespace zoom_range

/// Range and step of the per-file gain slider in decibels.
namespace gain_range
{
/// Largest boost or cut in dB the gain slider allows in either direction.
constexpr double maximumDecibels {12.0};
/// Step size in dB for the slider and for mouse wheel adjustments over the waveform.
constexpr double stepDecibels {0.1};
}  // namespace gain_range

}  // namespace audiobatch
