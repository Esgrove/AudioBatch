/// Table model for the sortable analysis results table in the GUI.
/// Declares AudioFileTableModel, a juce::TableListBoxModel with column identifiers, compile-time column widths,
/// sorting, cell painting, and row actions for the analyzed files.

#pragma once

#include "AudioAnalysis.h"

#include <JuceHeader.h>

#include <cstdint>
#include <functional>
#include <vector>

namespace audiobatch
{
/// Outcome of the last normalize or process run for a file, shown in the status column while the file is idle.
struct OperationMessage {
    /// Message shown in the status column.
    juce::String text;
    /// True for a failure, which is highlighted, and false for an informational notice.
    bool isError = false;
};

/// Stable identifiers shared by the header configuration, painting, and sort logic.
enum AudioFileTableColumn : std::uint8_t {
    columnName = 1,
    columnPath,
    columnType,
    columnBitrate,
    columnSampleRate,
    columnPeakLeft,
    columnPeakRight,
    columnOverallPeak,
    columnOverallTruePeak,
    columnMaxShortTermLufs,
    columnIntegratedLufs,
    columnCustomGain,
    columnStatus,
};

/// Table model for analyzed audio files, including sorting, painting, and row actions.
class AudioFileTableModel : public juce::TableListBoxModel
{
public:
    AudioFileTableModel(
        const std::vector<AudioAnalysisRecord>& sourceRecords,
        std::function<void(int row)> onSelectionChanged,
        std::function<void(int columnId, bool isForwards)> onSortChanged,
        std::function<void(int row, int columnId, const juce::MouseEvent& event)> onContextMenuRequested,
        std::function<juce::String(const AudioAnalysisRecord& record)> statusLabelProvider,
        std::function<OperationMessage(const AudioAnalysisRecord& record)> messageProvider,
        std::function<float()> phaseProvider
    );

    /// Returns the compile-time default width for a column.
    /// The defaults are chosen so that all columns together exactly fill the default window width,
    /// which is enforced by a static_assert below.
    static consteval int initialColumnWidth(const AudioFileTableColumn columnId)
    {
        switch (columnId) {
            case columnName:
                return nameColumnDefaultWidth;
            case columnPath:
                return pathColumnDefaultWidth;
            case columnType:
                return typeColumnDefaultWidth;
            case columnBitrate:
                return bitrateColumnDefaultWidth;
            case columnSampleRate:
                return sampleRateColumnDefaultWidth;
            case columnOverallPeak:
                return overallPeakColumnDefaultWidth;
            case columnPeakLeft:
                return peakLeftColumnDefaultWidth;
            case columnPeakRight:
                return peakRightColumnDefaultWidth;
            case columnOverallTruePeak:
                return truePeakColumnDefaultWidth;
            case columnMaxShortTermLufs:
                return maxShortTermLufsColumnDefaultWidth;
            case columnIntegratedLufs:
                return integratedLufsColumnDefaultWidth;
            case columnCustomGain:
                return customGainColumnDefaultWidth;
            case columnStatus:
                return statusColumnDefaultWidth;
        }

        return 0;
    }

    /// Returns the compile-time minimum width a user can shrink a column to.
    /// All metric columns share one minimum so their numeric values stay readable.
    static consteval int minimumColumnWidth(const AudioFileTableColumn columnId)
    {
        switch (columnId) {
            case columnName:
                return nameColumnMinimumWidth;
            case columnPath:
                return pathColumnMinimumWidth;
            case columnType:
                return typeColumnMinimumWidth;
            case columnBitrate:
                return bitrateColumnMinimumWidth;
            case columnSampleRate:
                return sampleRateColumnMinimumWidth;
            case columnOverallPeak:
            case columnPeakLeft:
            case columnPeakRight:
            case columnOverallTruePeak:
            case columnMaxShortTermLufs:
            case columnIntegratedLufs:
            case columnCustomGain:
            case columnStatus:
                return metricColumnMinimumWidth;
        }

        return 0;
    }

    static void configureHeader(juce::TableHeaderComponent& header);

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

    static constexpr int defaultWindowWidth = 1096;
    static constexpr int nameColumnMinimumWidth = 110;
    static constexpr int pathColumnMinimumWidth = 140;
    static constexpr int typeColumnMinimumWidth = 60;
    static constexpr int bitrateColumnMinimumWidth = 68;
    static constexpr int sampleRateColumnMinimumWidth = 72;
    static constexpr int metricColumnMinimumWidth = 72;
    static constexpr int nameColumnDefaultWidth = 140;
    static constexpr int pathColumnDefaultWidth = 160;
    static constexpr int typeColumnDefaultWidth = 66;
    static constexpr int bitrateColumnDefaultWidth = 70;
    static constexpr int sampleRateColumnDefaultWidth = 76;
    static constexpr int overallPeakColumnDefaultWidth = 74;
    static constexpr int peakLeftColumnDefaultWidth = 74;
    static constexpr int peakRightColumnDefaultWidth = 74;
    static constexpr int truePeakColumnDefaultWidth = 74;
    static constexpr int maxShortTermLufsColumnDefaultWidth = 76;
    static constexpr int integratedLufsColumnDefaultWidth = 76;
    static constexpr int customGainColumnDefaultWidth = 72;
    static constexpr int statusColumnDefaultWidth = 64;
    static constexpr int totalInitialColumnWidth = nameColumnDefaultWidth + pathColumnDefaultWidth
        + typeColumnDefaultWidth + bitrateColumnDefaultWidth + sampleRateColumnDefaultWidth
        + overallPeakColumnDefaultWidth + peakLeftColumnDefaultWidth + peakRightColumnDefaultWidth
        + truePeakColumnDefaultWidth + maxShortTermLufsColumnDefaultWidth + integratedLufsColumnDefaultWidth
        + customGainColumnDefaultWidth + statusColumnDefaultWidth;

    static_assert(
        totalInitialColumnWidth == defaultWindowWidth,
        "Initial column widths must sum to default window width"
    );

    const std::vector<AudioAnalysisRecord>* records;
    std::function<void(int row)> selectionChanged;
    std::function<void(int columnId, bool isForwards)> sortChanged;
    std::function<void(int row, int columnId, const juce::MouseEvent& event)> contextMenuRequested;
    std::function<juce::String(const AudioAnalysisRecord& record)> activeStatusLabelProvider;
    std::function<OperationMessage(const AudioAnalysisRecord& record)> operationMessageProvider;
    std::function<float()> activityPhaseProvider;
};
}  // namespace audiobatch
