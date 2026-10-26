/// Implementation of the results table sorting:
/// the secondary sort table header and the per-column record comparators.

#include "gui/ResultsSorting.h"

#include "analysis/AudioAnalysis.h"
#include "analysis/AudioAnalysisFormatting.h"
#include "gui/GuiLayout.h"
#include "utils/StringFormat.h"

#include <algorithm>
#include <cmath>
#include <compare>
#include <concepts>
#include <utility>

namespace audiobatch
{
/// Stores the callback invoked when a column is clicked with ctrl or cmd held down.
SecondarySortTableHeader::SecondarySortTableHeader(SecondarySortCallback secondarySortRequestedCallback) :
    secondarySortRequested(std::move(secondarySortRequestedCallback))
{}

/// Routes ctrl or cmd clicks to the secondary sort callback,
/// while plain clicks fall through to the normal primary sort behaviour.
void SecondarySortTableHeader::columnClicked(const int columnId, const juce::ModifierKeys& modifiers)
{
    if ((modifiers.isCtrlDown() || modifiers.isCommandDown()) && secondarySortRequested != nullptr) {
        if (const auto column = tableColumnFromId(columnId)) {
            secondarySortRequested(*column);
        }
        return;
    }

    juce::TableHeaderComponent::columnClicked(columnId, modifiers);
}

/// Appends a textual secondary sort marker to the given column's name and restores the base name of every other column.
/// Passing no column clears the indicator entirely.
void SecondarySortTableHeader::setSecondarySortIndicator(
    const std::optional<AudioFileTableColumn> column,
    const bool isForwards
)
{
    if (baseColumnNames.empty()) {
        cacheBaseColumnNames();
    }

    secondarySortColumn = column;
    secondarySortForwards = isForwards;

    for (const auto& [trackedColumnId, baseName] : baseColumnNames) {
        auto displayName = baseName;

        if (trackedColumnId == toColumnId(secondarySortColumn)) {
            displayName = utils::format("{} {}", displayName, secondarySortForwards ? "[2^]" : "[2v]");
        }

        setColumnName(trackedColumnId, displayName);
    }

    repaint();
}

/// Captures the original column names once, so indicator suffixes can be re-applied without accumulating in the names.
void SecondarySortTableHeader::cacheBaseColumnNames()
{
    const auto columnCount = getNumColumns(false);

    for (int index = 0; index < columnCount; ++index) {
        const auto columnId = getColumnIdOfIndex(index, false);
        baseColumnNames.emplace(columnId, getColumnName(columnId));
    }
}

namespace results_sorting
{
namespace
{
/// Three-way natural-order string comparison, so numbered file names sort as expected.
int compareNatural(const juce::String& left, const juce::String& right)
{
    return left.compareNatural(right);
}

/// Three-way comparison of floating point values using the IEEE total order.
/// Only identical values compare equal, because a tolerance based equality is not transitive
/// and would break the strict weak ordering that std::ranges::sort requires.
template<std::floating_point Value>
int compareValues(const Value left, const Value right)
{
    const auto ordering = std::strong_order(left, right);

    if (ordering < 0) {
        return -1;
    }

    return ordering > 0 ? 1 : 0;
}

/// Three-way comparison of peak values by magnitude, ignoring polarity.
template<std::floating_point Value>
int comparePeaks(const Value left, const Value right)
{
    return compareValues(std::abs(left), std::abs(right));
}

/// Three-way comparison by average bitrate.
int compareBitrates(const AudioAnalysisRecord& lhs, const AudioAnalysisRecord& rhs)
{
    const auto leftBitrate = getAverageBitrateKbps(lhs);
    const auto rightBitrate = getAverageBitrateKbps(rhs);

    return compareValues(leftBitrate, rightBitrate);
}

/// Three-way comparison by sample rate.
int compareSampleRates(const AudioAnalysisRecord& lhs, const AudioAnalysisRecord& rhs)
{
    if (lhs.sampleRate == rhs.sampleRate) {
        return 0;
    }

    return lhs.sampleRate < rhs.sampleRate ? -1 : 1;
}

/// Three-way loudness comparison that sorts negative-infinity readings below every measurable value.
int compareLoudness(const double left, const double right)
{
    const auto leftIsNegativeInfinity = left <= AudioAnalysisRecord::negativeInfinityLoudness;
    const auto rightIsNegativeInfinity = right <= AudioAnalysisRecord::negativeInfinityLoudness;

    if (leftIsNegativeInfinity && rightIsNegativeInfinity) {
        return 0;
    }

    if (leftIsNegativeInfinity != rightIsNegativeInfinity) {
        return leftIsNegativeInfinity ? -1 : 1;
    }

    return compareValues(left, right);
}
}  // namespace

/// Returns a negative, zero, or positive value as lhs sorts before, with, or after rhs in the given table column.
/// Peaks compare by magnitude, and silent loudness sorts below every measurable value.
int compareRecordsByColumn(
    const AudioAnalysisRecord& lhs,
    const AudioAnalysisRecord& rhs,
    const AudioFileTableColumn column
)
{
    switch (column) {
        case AudioFileTableColumn::name:
            return compareNatural(lhs.fileName, rhs.fileName);

        case AudioFileTableColumn::path:
            return compareNatural(lhs.fullPath, rhs.fullPath);

        case AudioFileTableColumn::type:
            return compareNatural(formatTypeLabel(lhs), formatTypeLabel(rhs));

        case AudioFileTableColumn::bitDepth:
            if (lhs.bitsPerSample == rhs.bitsPerSample) {
                return 0;
            }
            return lhs.bitsPerSample < rhs.bitsPerSample ? -1 : 1;

        case AudioFileTableColumn::bitrate:
            return compareBitrates(lhs, rhs);

        case AudioFileTableColumn::sampleRate:
            return compareSampleRates(lhs, rhs);

        case AudioFileTableColumn::peakLeft:
            return comparePeaks(lhs.peakLeft, rhs.peakLeft);

        case AudioFileTableColumn::peakRight:
            return comparePeaks(lhs.peakRight, rhs.peakRight);

        case AudioFileTableColumn::overallTruePeak:
            return comparePeaks(lhs.overallTruePeak, rhs.overallTruePeak);

        case AudioFileTableColumn::maxShortTermLufs:
            return compareLoudness(lhs.maxShortTermLufs, rhs.maxShortTermLufs);

        case AudioFileTableColumn::integratedLufs:
            return compareLoudness(lhs.integratedLufs, rhs.integratedLufs);

        case AudioFileTableColumn::customGain: {
            if (lhs.hasCustomGain != rhs.hasCustomGain) {
                return lhs.hasCustomGain ? 1 : -1;
            }
            if (!lhs.hasCustomGain) {
                return 0;
            }
            return compareValues(lhs.customGainDb, rhs.customGainDb);
        }

        case AudioFileTableColumn::status:
            return compareNatural(formatStatus(lhs), formatStatus(rhs));

        case AudioFileTableColumn::overallPeak:
            return comparePeaks(lhs.overallPeak, rhs.overallPeak);
    }
    return 0;
}

/// Sorts the records for the results table.
/// Failed records always come last, then the primary and secondary columns decide,
/// and the file name and path break any remaining ties so the order is stable.
void sortRecords(std::vector<AudioAnalysisRecord>& records, const SortState& state)
{
    std::ranges::sort(records, [&state](const auto& lhs, const auto& rhs) {
        if (lhs.hasError() != rhs.hasError()) {
            return !lhs.hasError();
        }

        const auto compareUsingSort = [&](const std::optional<AudioFileTableColumn> column, const bool isForwards) {
            if (!column.has_value()) {
                return 0;
            }

            const auto comparison = column == AudioFileTableColumn::status && state.statusText != nullptr
                ? compareNatural(state.statusText(lhs), state.statusText(rhs))
                : compareRecordsByColumn(lhs, rhs, *column);
            return isForwards ? comparison : -comparison;
        };

        if (const auto primaryComparison = compareUsingSort(state.primaryColumn, state.primaryForwards);
            primaryComparison != 0)
        {
            return primaryComparison < 0;
        }

        if (const auto secondaryComparison = compareUsingSort(state.secondaryColumn, state.secondaryForwards);
            secondaryComparison != 0)
        {
            return secondaryComparison < 0;
        }

        if (const auto nameComparison = compareNatural(lhs.fileName, rhs.fileName); nameComparison != 0) {
            return nameComparison < 0;
        }

        return compareNatural(lhs.fullPath, rhs.fullPath) < 0;
    });
}

/// Applies a new primary sort column and direction.
/// Clears the secondary sort when it targets the same column, since sorting twice by one column adds nothing.
void setPrimarySort(SortState& state, const std::optional<AudioFileTableColumn> column, const bool isForwards)
{
    state.primaryColumn = column;
    state.primaryForwards = isForwards;

    if (state.secondaryColumn == state.primaryColumn) {
        state.secondaryColumn.reset();
        state.secondaryForwards = true;
    }
}

/// Sets, toggles, or clears the secondary sort column for a ctrl or cmd click on a header.
/// Clicking the primary column clears the secondary sort,
/// clicking the current secondary column flips its direction,
/// and clicking any other column makes it the ascending secondary sort.
void toggleSecondarySort(SortState& state, const AudioFileTableColumn column)
{
    if (column == state.primaryColumn) {
        state.secondaryColumn.reset();
        state.secondaryForwards = true;
    } else if (state.secondaryColumn == column) {
        state.secondaryForwards = !state.secondaryForwards;
    } else {
        state.secondaryColumn = column;
        state.secondaryForwards = true;
    }
}

/// Returns the table's header downcast to SecondarySortTableHeader.
/// The table must have been created with that header type.
SecondarySortTableHeader& getSecondarySortHeader(const juce::TableListBox& table)
{
    return dynamic_cast<SecondarySortTableHeader&>(table.getHeader());
}
}  // namespace results_sorting
}  // namespace audiobatch
