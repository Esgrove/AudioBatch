/// Sorting of the GUI results table.
/// Declares SecondarySortTableHeader, the table header that adds a ctrl or cmd click secondary sort,
/// and the results_sorting functions that compare records by table column and sort the result list.

#pragma once

#include "AudioAnalysisTypes.h"

#include <JuceHeader.h>

#include <functional>
#include <map>
#include <vector>

/// Table header that maps ctrl or cmd clicks to a secondary sort request
/// and renders the secondary sort indicator inside the column name text.
class SecondarySortTableHeader final : public juce::TableHeaderComponent
{
public:
    /// Called with the column id when a column is clicked with ctrl or cmd held down.
    using SecondarySortCallback = std::function<void(int columnId)>;

    /// Stores the callback invoked when a column is clicked with ctrl or cmd held down.
    explicit SecondarySortTableHeader(SecondarySortCallback secondarySortRequestedCallback);

    /// Routes ctrl or cmd clicks to the secondary sort callback,
    /// while plain clicks fall through to the normal primary sort behaviour.
    void columnClicked(int columnId, const juce::ModifierKeys& modifiers) override;

    /// Appends a textual secondary sort marker to the given column's name
    /// and restores the base name of every other column.
    /// Passing a column id of zero clears the indicator entirely.
    void setSecondarySortIndicator(int columnId, bool isForwards);

private:
    /// Captures the original column names once,
    /// so indicator suffixes can be re-applied without accumulating in the names.
    void cacheBaseColumnNames();

    SecondarySortCallback secondarySortRequested;
    std::map<int, juce::String> baseColumnNames;
    int secondarySortColumnId = 0;
    bool secondarySortForwards = true;
};

/// Comparators and sorting for the results table columns.
namespace results_sorting
{
/// The primary and secondary sort columns of the results table.
/// A column id of zero means no sort on that level.
struct SortState {
    int primaryColumnId = 0;
    bool primaryForwards = true;
    int secondaryColumnId = 0;
    bool secondaryForwards = true;
};

/// Returns a negative, zero, or positive value as lhs sorts before, with, or after rhs in the given table column.
/// Peaks compare by magnitude, silent loudness sorts below every measurable value,
/// and unknown column ids compare by overall peak.
int compareRecordsByColumn(const AudioAnalysisRecord& lhs, const AudioAnalysisRecord& rhs, int columnId);

/// Sorts the records for the results table.
/// Failed records always come last, then the primary and secondary columns decide,
/// and the file name and path break any remaining ties so the order is stable.
void sortRecords(std::vector<AudioAnalysisRecord>& records, const SortState& state);

/// Returns the table's header downcast to SecondarySortTableHeader.
/// The table must have been created with that header type.
SecondarySortTableHeader& getSecondarySortHeader(const juce::TableListBox& table);
}  // namespace results_sorting
