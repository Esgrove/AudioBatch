/// Results table header with a secondary sort gesture, indicator and per-column tooltip.

#pragma once

#include "gui/AudioBatchComponentConstants.h"

#include <JuceHeader.h>

#include <functional>
#include <map>
#include <optional>

namespace audiobatch
{
/// Maps ctrl or cmd clicks to secondary sorting and decorates the secondary column title.
class SecondarySortTableHeader final : public juce::TableHeaderComponent, public juce::TooltipClient
{
public:
    /// Describes the column under the mouse.
    using ColumnDescriber = std::function<juce::String(AudioFileTableColumn column)>;
    /// Receives the ctrl or cmd clicked column.
    using SecondarySortCallback = std::function<void(AudioFileTableColumn column)>;

    SecondarySortTableHeader(SecondarySortCallback secondarySortRequestedCallback, ColumnDescriber columnDescriber);

    void setSecondarySortIndicator(std::optional<AudioFileTableColumn> column, bool isForwards);

    // juce::TableHeaderComponent
    void columnClicked(int columnId, const juce::ModifierKeys& modifiers) override;

    // juce::TooltipClient
    [[nodiscard]] juce::String getTooltip() override;

private:
    void cacheBaseColumnNames();

    /// Describes the column under the mouse.
    ColumnDescriber describeColumn;
    /// Receives secondary sort requests.
    SecondarySortCallback secondarySortRequested;

    /// Original titles, keyed by JUCE column id.
    std::map<int, juce::String> baseColumnNames;
    /// Column carrying the secondary indicator, empty when absent.
    std::optional<AudioFileTableColumn> secondarySortColumn;

    /// Direction marked in the title.
    bool secondarySortForwards {true};
};

namespace results_sorting
{
SecondarySortTableHeader& getSecondarySortHeader(const juce::TableListBox& table);
}
}  // namespace audiobatch
