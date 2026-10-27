/// Table model for the sortable analysis results table in the GUI.
/// Declares AudioFileTableModel, a juce::TableListBoxModel with sorting, cell painting, and row actions.
/// Column identifiers and sizing rules are shared through GuiLayout.h.

#pragma once

#include "analysis/AudioAnalysis.h"
#include "gui/GuiLayout.h"
#include "gui/ResultsModel.h"

#include <JuceHeader.h>

#include <cstdint>
#include <functional>
#include <optional>

namespace audiobatch
{
/// What a busy file is doing, shown with an activity indicator in the status column.
enum class FileActivity : std::uint8_t {
    /// Queued for analysis, normalization, or processing, but not started yet.
    waiting,
    analyzing,
    normalizing,
    processing,
};

/// Activity of a busy file and how far along it is.
struct FileActivityState {
    /// What is happening to the file.
    FileActivity activity {FileActivity::waiting};
    /// Completed fraction in the range 0 to 1, initialized to zero for each activity.
    /// An empty optional represents indeterminate progress.
    std::optional<float> progress;
};

/// Outcome of the last normalize or process run for a file, shown in the status column while the file is idle.
struct OperationMessage {
    /// Message shown in the status column.
    juce::String text;
    /// True for a failure, which is highlighted, and false for an informational notice.
    bool isError {false};
};

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

    static void configureHeader(juce::TableHeaderComponent& header);
    [[nodiscard]] static juce::String getColumnDescription(AudioFileTableColumn column);

    int getNumRows() override;

    juce::var getDragSourceDescription(const juce::SparseSet<int>& currentlySelectedRows) override;

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

    void cellClicked(int rowNumber, int columnId, const juce::MouseEvent& event) override;

    void selectedRowsChanged(int lastRowSelected) override;

    void sortOrderChanged(int newSortColumnId, bool isForwards) override;

    juce::String getCellTooltip(int rowNumber, int columnId) override;

private:
    [[nodiscard]] OperationMessage getOperationMessage(const AudioAnalysisRecord& record) const;

    /// The rows of the table, owned by the component that owns this model and declared before it.
    const ResultsModel& records;
    std::function<void(int row)> selectionChanged;
    std::function<void(std::optional<AudioFileTableColumn> column, bool isForwards)> sortChanged;
    std::function<void(int row, const juce::MouseEvent& event)> contextMenuRequested;
    std::function<std::optional<FileActivityState>(const AudioAnalysisRecord& record)> activityStateProvider;
    std::function<OperationMessage(const AudioAnalysisRecord& record)> operationMessageProvider;
    std::function<float()> activityPhaseProvider;
};
}  // namespace audiobatch
