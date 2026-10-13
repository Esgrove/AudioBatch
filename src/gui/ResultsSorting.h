/// Sorting of the GUI results table.
/// Declares SecondarySortTableHeader, the table header that adds a ctrl or cmd click secondary sort,
/// and the results_sorting functions that compare records by table column and sort the result list.

#pragma once

#include "analysis/AudioAnalysisTypes.h"

#include <JuceHeader.h>

#include <functional>
#include <map>
#include <vector>

namespace audiobatch
{
/// Table header that maps ctrl or cmd clicks to a secondary sort request
/// and renders the secondary sort indicator inside the column name text.
class SecondarySortTableHeader final : public juce::TableHeaderComponent
{
public:
    /// Called with the column id when a column is clicked with ctrl or cmd held down.
    using SecondarySortCallback = std::function<void(int columnId)>;

    explicit SecondarySortTableHeader(SecondarySortCallback secondarySortRequestedCallback);

    void columnClicked(int columnId, const juce::ModifierKeys& modifiers) override;
    void setSecondarySortIndicator(int columnId, bool isForwards);

private:
    void cacheBaseColumnNames();

    SecondarySortCallback secondarySortRequested;
    bool secondarySortForwards = true;
    int secondarySortColumnId = 0;
    std::map<int, juce::String> baseColumnNames;
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
    /// Returns the text the status column shows for a record, such as the message of its last operation.
    /// The status column sorts by this text when set, and by the analysis status otherwise.
    std::function<juce::String(const AudioAnalysisRecord&)> statusText;
};

int compareRecordsByColumn(const AudioAnalysisRecord& lhs, const AudioAnalysisRecord& rhs, int columnId);

void setPrimarySort(SortState& state, int columnId, bool isForwards);
void sortRecords(std::vector<AudioAnalysisRecord>& records, const SortState& state);
void toggleSecondarySort(SortState& state, int columnId);

SecondarySortTableHeader& getSecondarySortHeader(const juce::TableListBox& table);
}  // namespace results_sorting
}  // namespace audiobatch
