/// Implementation of the results table sorting:
/// the secondary sort table header and the per-column record comparators.

#include "ResultsSorting.h"

#include "AudioAnalysisService.h"
#include "AudioFileTableModel.h"
#include "StringFormat.h"

#include <algorithm>
#include <cmath>
#include <compare>
#include <concepts>
#include <utility>

SecondarySortTableHeader::SecondarySortTableHeader(SecondarySortCallback secondarySortRequestedCallback) :
    secondarySortRequested(std::move(secondarySortRequestedCallback))
{}

void SecondarySortTableHeader::columnClicked(const int columnId, const juce::ModifierKeys& modifiers)
{
    if ((modifiers.isCtrlDown() || modifiers.isCommandDown()) && secondarySortRequested != nullptr) {
        secondarySortRequested(columnId);
        return;
    }

    juce::TableHeaderComponent::columnClicked(columnId, modifiers);
}

void SecondarySortTableHeader::setSecondarySortIndicator(const int columnId, const bool isForwards)
{
    if (baseColumnNames.empty()) {
        cacheBaseColumnNames();
    }

    secondarySortColumnId = columnId;
    secondarySortForwards = isForwards;

    for (const auto& [trackedColumnId, baseName] : baseColumnNames) {
        auto displayName = baseName;

        if (trackedColumnId == secondarySortColumnId) {
            displayName = utils::format("{} {}", displayName, secondarySortForwards ? "[2^]" : "[2v]");
        }

        setColumnName(trackedColumnId, displayName);
    }

    repaint();
}

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
/// Three-way natural-order string comparison, so numbered file names sort as expected.
static int compareNatural(const juce::String& left, const juce::String& right)
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
static int compareBitrates(const AudioAnalysisRecord& lhs, const AudioAnalysisRecord& rhs)
{
    const auto leftBitrate = AudioAnalysisService::getAverageBitrateKbps(lhs);
    const auto rightBitrate = AudioAnalysisService::getAverageBitrateKbps(rhs);

    return compareValues(leftBitrate, rightBitrate);
}

/// Three-way comparison by sample rate.
static int compareSampleRates(const AudioAnalysisRecord& lhs, const AudioAnalysisRecord& rhs)
{
    if (lhs.sampleRate == rhs.sampleRate) {
        return 0;
    }

    return lhs.sampleRate < rhs.sampleRate ? -1 : 1;
}

/// Three-way loudness comparison that sorts negative-infinity readings below every measurable value.
static int compareLoudness(const double left, const double right)
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

int compareRecordsByColumn(const AudioAnalysisRecord& lhs, const AudioAnalysisRecord& rhs, const int columnId)
{
    switch (columnId) {
        case AudioFileTableModel::columnName:
            return compareNatural(lhs.fileName, rhs.fileName);

        case AudioFileTableModel::columnPath:
            return compareNatural(lhs.fullPath, rhs.fullPath);

        case AudioFileTableModel::columnType:
            return compareNatural(
                AudioAnalysisService::formatTypeLabel(lhs), AudioAnalysisService::formatTypeLabel(rhs)
            );

        case AudioFileTableModel::columnBitrate:
            return compareBitrates(lhs, rhs);

        case AudioFileTableModel::columnSampleRate:
            return compareSampleRates(lhs, rhs);

        case AudioFileTableModel::columnPeakLeft:
            return comparePeaks(lhs.peakLeft, rhs.peakLeft);

        case AudioFileTableModel::columnPeakRight:
            return comparePeaks(lhs.peakRight, rhs.peakRight);

        case AudioFileTableModel::columnOverallTruePeak:
            return comparePeaks(lhs.overallTruePeak, rhs.overallTruePeak);

        case AudioFileTableModel::columnMaxShortTermLufs:
            return compareLoudness(lhs.maxShortTermLufs, rhs.maxShortTermLufs);

        case AudioFileTableModel::columnIntegratedLufs:
            return compareLoudness(lhs.integratedLufs, rhs.integratedLufs);

        case AudioFileTableModel::columnCustomGain: {
            if (lhs.hasCustomGain != rhs.hasCustomGain) {
                return lhs.hasCustomGain ? 1 : -1;
            }
            if (!lhs.hasCustomGain) {
                return 0;
            }
            return compareValues(lhs.customGainDb, rhs.customGainDb);
        }

        case AudioFileTableModel::columnStatus:
            return compareNatural(AudioAnalysisService::formatStatus(lhs), AudioAnalysisService::formatStatus(rhs));

        case AudioFileTableModel::columnOverallPeak:
        default:
            return comparePeaks(lhs.overallPeak, rhs.overallPeak);
    }
}

void sortRecords(std::vector<AudioAnalysisRecord>& records, const SortState& state)
{
    std::ranges::sort(records, [&state](const auto& lhs, const auto& rhs) {
        if (lhs.hasError() != rhs.hasError()) {
            return !lhs.hasError();
        }

        const auto compareUsingSort = [&](const int columnId, const bool isForwards) {
            if (columnId <= 0) {
                return 0;
            }

            const auto comparison = columnId == AudioFileTableModel::columnStatus && state.statusText != nullptr
                ? compareNatural(state.statusText(lhs), state.statusText(rhs))
                : compareRecordsByColumn(lhs, rhs, columnId);
            return isForwards ? comparison : -comparison;
        };

        if (const auto primaryComparison = compareUsingSort(state.primaryColumnId, state.primaryForwards);
            primaryComparison != 0)
        {
            return primaryComparison < 0;
        }

        if (const auto secondaryComparison = compareUsingSort(state.secondaryColumnId, state.secondaryForwards);
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

SecondarySortTableHeader& getSecondarySortHeader(const juce::TableListBox& table)
{
    return dynamic_cast<SecondarySortTableHeader&>(table.getHeader());
}
}  // namespace results_sorting
