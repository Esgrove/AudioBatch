#pragma once

#include "AudioAnalysisService.h"

#include <JuceHeader.h>

#include <functional>
#include <vector>

/// Table model for analyzed audio files, including sorting, painting, and row actions.
class AudioFileTableModel : public juce::TableListBoxModel
{
public:
    /// Stable identifiers shared by the header configuration, painting, and sort logic.
    enum ColumnId {
        columnName = 1,
        columnPath,
        columnType,
        columnBitrate,
        columnPeakLeft,
        columnPeakRight,
        columnOverallPeak,
        columnOverallTruePeak,
        columnMaxShortTermLufs,
        columnIntegratedLufs,
        columnStatus,
    };

    static consteval int initialColumnWidth(const ColumnId columnId)
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
            case columnStatus:
                return statusColumnDefaultWidth;
        }

        return 0;
    }

    static consteval int minimumColumnWidth(const ColumnId columnId)
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
            case columnOverallPeak:
            case columnPeakLeft:
            case columnPeakRight:
            case columnOverallTruePeak:
            case columnMaxShortTermLufs:
            case columnIntegratedLufs:
            case columnStatus:
                return metricColumnMinimumWidth;
        }

        return 0;
    }

    /// Creates a model backed by the live result list and UI callbacks owned by the main view.
    AudioFileTableModel(
        std::vector<AudioAnalysisRecord>& records,
        std::function<void(int row)> selectionChanged,
        std::function<void(int columnId, bool isForwards)> sortChanged,
        std::function<void(int row, int columnId, const juce::MouseEvent& event)> contextMenuRequested,
        std::function<juce::String(const AudioAnalysisRecord& record)> activeStatusLabelProvider,
        std::function<float()> activityPhaseProvider
    );

    /// Installs the standard set of columns used by the results table.
    static void configureHeader(juce::TableHeaderComponent& header);

    /// Returns the number of analyzed rows currently available.
    int getNumRows() override;

    /// Packages the selected file paths for external drag-and-drop.
    juce::var getDragSourceDescription(const juce::SparseSet<int>& currentlySelectedRows) override;

    /// Paints the text content for a single table cell.
    void paintCell(juce::Graphics& g, int rowNumber, int columnId, int width, int height, bool rowIsSelected) override;

    /// Paints alternating row backgrounds and selection highlights.
    void paintRowBackground(juce::Graphics& g, int rowNumber, int width, int height, bool rowIsSelected) override;

    /// Forwards popup-clicks so the owning component can show file actions.
    void cellClicked(int rowNumber, int columnId, const juce::MouseEvent& event) override;

    /// Notifies the owning component when the row selection changes.
    void selectedRowsChanged(int lastRowSelected) override;

    /// Notifies the owning component when the active sort column changes.
    void sortOrderChanged(int newSortColumnId, bool isForwards) override;

    /// Suppresses cell tooltips for the main results list.
    juce::String getCellTooltip(int rowNumber, int columnId) override;

private:
    static constexpr int defaultWindowWidth = 1024;
    static constexpr int nameColumnMinimumWidth = 110;
    static constexpr int pathColumnMinimumWidth = 140;
    static constexpr int typeColumnMinimumWidth = 60;
    static constexpr int bitrateColumnMinimumWidth = 68;
    static constexpr int metricColumnMinimumWidth = 74;
    static constexpr int nameColumnDefaultWidth = 160;
    static constexpr int pathColumnDefaultWidth = 190;
    static constexpr int typeColumnDefaultWidth = 70;
    static constexpr int bitrateColumnDefaultWidth = 74;
    static constexpr int overallPeakColumnDefaultWidth = 76;
    static constexpr int peakLeftColumnDefaultWidth = 76;
    static constexpr int peakRightColumnDefaultWidth = 76;
    static constexpr int truePeakColumnDefaultWidth = 76;
    static constexpr int maxShortTermLufsColumnDefaultWidth = 78;
    static constexpr int integratedLufsColumnDefaultWidth = 78;
    static constexpr int statusColumnDefaultWidth = 70;
    static constexpr int totalInitialColumnWidth = nameColumnDefaultWidth + pathColumnDefaultWidth
        + typeColumnDefaultWidth + bitrateColumnDefaultWidth + overallPeakColumnDefaultWidth
        + peakLeftColumnDefaultWidth + peakRightColumnDefaultWidth + truePeakColumnDefaultWidth
        + maxShortTermLufsColumnDefaultWidth + integratedLufsColumnDefaultWidth + statusColumnDefaultWidth;

    static_assert(
        totalInitialColumnWidth == defaultWindowWidth,
        "Initial column widths must sum to default window width"
    );

    std::vector<AudioAnalysisRecord>& records;
    std::function<void(int row)> selectionChanged;
    std::function<void(int columnId, bool isForwards)> sortChanged;
    std::function<void(int row, int columnId, const juce::MouseEvent& event)> contextMenuRequested;
    std::function<juce::String(const AudioAnalysisRecord& record)> activeStatusLabelProvider;
    std::function<float()> activityPhaseProvider;
};
