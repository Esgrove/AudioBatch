/// Sorting of the GUI results table.
/// Declares SecondarySortTableHeader, the table header that adds a ctrl or cmd click secondary sort
/// and describes the column under the mouse in a tooltip,
/// and the results_sorting functions that compare records by table column and sort the result list.

#pragma once

#include "analysis/AudioAnalysisTypes.h"
#include "gui/AudioBatchComponentConstants.h"

#include <JuceHeader.h>

#include <functional>
#include <map>
#include <optional>
#include <vector>

namespace audiobatch
{
/// Table header that maps ctrl or cmd clicks to a secondary sort request
/// and renders the secondary sort indicator inside the column name text.
/// Its tooltip describes the column under the mouse, with the unit the short column titles leave out.
class SecondarySortTableHeader final : public juce::TableHeaderComponent, public juce::TooltipClient
{
public:
    /// Called with the column when a column is clicked with ctrl or cmd held down.
    using SecondarySortCallback = std::function<void(AudioFileTableColumn column)>;

    /// Returns the description of a column for the header tooltip.
    using ColumnDescriber = std::function<juce::String(AudioFileTableColumn column)>;

    SecondarySortTableHeader(SecondarySortCallback secondarySortRequestedCallback, ColumnDescriber columnDescriber);

    void columnClicked(int columnId, const juce::ModifierKeys& modifiers) override;
    void setSecondarySortIndicator(std::optional<AudioFileTableColumn> column, bool isForwards);

    [[nodiscard]] juce::String getTooltip() override;

private:
    void cacheBaseColumnNames();

    SecondarySortCallback secondarySortRequested;
    ColumnDescriber describeColumn;
    bool secondarySortForwards {true};
    std::optional<AudioFileTableColumn> secondarySortColumn;
    /// Column names without the secondary sort marker, keyed by JUCE column id.
    std::map<int, juce::String> baseColumnNames;
};

/// Comparators and sorting for the results table columns.
namespace results_sorting
{
/// The primary and secondary sort columns of the results table.
/// An empty column means no sort on that level.
struct SortState {
    std::optional<AudioFileTableColumn> primaryColumn;
    bool primaryForwards {true};
    std::optional<AudioFileTableColumn> secondaryColumn;
    bool secondaryForwards {true};
    /// Returns the text the status column shows for a record, such as the message of its last operation.
    /// The status column sorts by this text when set, and by the analysis status otherwise.
    std::function<juce::String(const AudioAnalysisRecord&)> statusText;
};

int compareRecordsByColumn(const AudioAnalysisRecord& lhs, const AudioAnalysisRecord& rhs, AudioFileTableColumn column);

void setPrimarySort(SortState& state, std::optional<AudioFileTableColumn> column, bool isForwards);
void sortRecords(std::vector<AudioAnalysisRecord>& records, const SortState& state);
void toggleSecondarySort(SortState& state, AudioFileTableColumn column);

SecondarySortTableHeader& getSecondarySortHeader(const juce::TableListBox& table);
}  // namespace results_sorting
}  // namespace audiobatch
