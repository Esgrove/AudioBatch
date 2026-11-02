/// Table model for the sortable analysis results table in the GUI.
/// Declares AudioFileTableModel, a juce::TableListBoxModel with sorting, cell painting, and row actions.
/// Column identifiers are shared through AudioBatchComponentConstants.h, and sizing rules through GuiLayout.h.

#pragma once

#include "analysis/AudioAnalysis.h"
#include "gui/FileActivity.h"
#include "gui/GuiLayout.h"
#include "gui/ResultsModel.h"

#include <JuceHeader.h>

#include <cstdint>
#include <functional>
#include <optional>

namespace audiobatch
{
/// Table model for analyzed audio files, including sorting, painting, and row actions.
class AudioFileTableModel : public juce::TableListBoxModel
{
public:
    AudioFileTableModel(
        const ResultsModel& sourceRecords,
        std::function<void(int row)> onSelectionChanged,
        std::function<void(std::optional<AudioFileTableColumn> column, bool isForwards)> onSortChanged,
        std::function<void(int row, const juce::MouseEvent& event)> onContextMenuRequested,
        std::function<std::optional<FileActivityState>(const AudioAnalysisRecord& record)> activityProvider,
        std::function<OperationMessage(const AudioAnalysisRecord& record)> messageProvider,
        std::function<float()> phaseProvider
    );

    ~AudioFileTableModel() override = default;

    AudioFileTableModel(const AudioFileTableModel&) = delete;
    AudioFileTableModel& operator=(const AudioFileTableModel&) = delete;
    AudioFileTableModel(AudioFileTableModel&&) = delete;
    AudioFileTableModel& operator=(AudioFileTableModel&&) = delete;

    [[nodiscard]] static juce::String getColumnDescription(AudioFileTableColumn column);
    static void configureHeader(juce::TableHeaderComponent& header);

    // juce::TableListBoxModel
    juce::String getCellTooltip(int rowNumber, int columnId) override;
    juce::var getDragSourceDescription(const juce::SparseSet<int>& currentlySelectedRows) override;

    int getNumRows() override;

    void cellClicked(int rowNumber, int columnId, const juce::MouseEvent& event) override;
    void paintCell(
        juce::Graphics& graphics,
        int rowNumber,
        int columnId,
        int width,
        int height,
        bool rowIsSelected
    ) override;
    void paintRowBackground(
        juce::Graphics& graphics,
        int rowNumber,
        int width,
        int height,
        bool rowIsSelected
    ) override;
    void selectedRowsChanged(int lastRowSelected) override;
    void sortOrderChanged(int newSortColumnId, bool isForwards) override;

private:
    [[nodiscard]] OperationMessage getOperationMessage(const AudioAnalysisRecord& record) const;

    /// The rows of the table, owned by the component that owns this model and declared before it.
    const ResultsModel& records;

    std::function<float()> activityPhaseProvider;
    std::function<OperationMessage(const AudioAnalysisRecord& record)> operationMessageProvider;
    std::function<std::optional<FileActivityState>(const AudioAnalysisRecord& record)> activityStateProvider;
    std::function<void(int row, const juce::MouseEvent& event)> contextMenuRequested;
    std::function<void(int row)> selectionChanged;
    std::function<void(std::optional<AudioFileTableColumn> column, bool isForwards)> sortChanged;
};
}  // namespace audiobatch
