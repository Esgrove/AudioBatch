/// Secondary sort header gestures, title decoration and column tooltips.

#include "gui/SecondarySortTableHeader.h"

#include "gui/AudioBatchComponentConstants.h"
#include "utils/StringFormat.h"

#include <optional>
#include <utility>

namespace audiobatch
{
/// Stores the sort request and tooltip callbacks.
SecondarySortTableHeader::SecondarySortTableHeader(
    SecondarySortCallback secondarySortRequestedCallback,
    ColumnDescriber columnDescriber
) :
    describeColumn(std::move(columnDescriber)),
    secondarySortRequested(std::move(secondarySortRequestedCallback))
{}

/// Describes the current column each time the tooltip is requested.
juce::String SecondarySortTableHeader::getTooltip()
{
    const auto column = tableColumnFromId(getColumnIdAtX(getMouseXYRelative().getX()));
    return column.has_value() && describeColumn != nullptr ? describeColumn(*column) : juce::String();
}

/// Routes modifier clicks to secondary sorting, leaving plain clicks to JUCE.
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

/// Adds the secondary marker to its column and restores the original titles elsewhere.
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

/// Captures original names once, preventing accumulated marker suffixes.
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
/// Returns the GUI's installed secondary-sort header.
SecondarySortTableHeader& getSecondarySortHeader(const juce::TableListBox& table)
{
    return dynamic_cast<SecondarySortTableHeader&>(table.getHeader());
}
}  // namespace results_sorting
}  // namespace audiobatch
