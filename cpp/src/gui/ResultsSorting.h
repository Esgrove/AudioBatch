/// Sort state and record comparison for the GUI results table.
/// Keeps primary and secondary sorting independent of the table header component.

#pragma once

#include "analysis/AudioAnalysisTypes.h"
#include "gui/AudioBatchComponentConstants.h"

#include <JuceHeader.h>

#include <functional>
#include <optional>
#include <vector>

namespace audiobatch::results_sorting
{
/// The primary and secondary sort columns of the results table.
/// An empty column means no sort on that level.
struct SortState {
    // Preserve the primary/secondary designated-initializer contract used by callers and tests.
    /// The column the rows are sorted by first.
    std::optional<AudioFileTableColumn> primaryColumn;
    /// True to sort the primary column in ascending order.
    bool primaryForwards {true};
    /// The column that orders the rows with equal primary values.
    std::optional<AudioFileTableColumn> secondaryColumn;
    /// True to sort the secondary column in ascending order.
    bool secondaryForwards {true};
    /// Returns the text the status column shows for a record, such as the message of its last operation.
    /// The status column sorts by this text when set, and by the analysis status otherwise.
    std::function<juce::String(const AudioAnalysisRecord&)> statusText;
};

int compareRecordsByColumn(const AudioAnalysisRecord& lhs, const AudioAnalysisRecord& rhs, AudioFileTableColumn column);

void setPrimarySort(SortState& state, std::optional<AudioFileTableColumn> column, bool isForwards);
void sortRecords(std::vector<AudioAnalysisRecord>& records, const SortState& state);
void toggleSecondarySort(SortState& state, AudioFileTableColumn column);
}  // namespace audiobatch::results_sorting
