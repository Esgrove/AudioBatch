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
        columnPeakLeft,
        columnPeakRight,
        columnOverallPeak,
        columnStatus,
    };

    static consteval int initialColumnWidth(ColumnId columnId)
    {
        switch (columnId) {
            case columnName:
                return widthFromDefaultWindowFraction(nameColumnWidthFraction);
            case columnPath:
                return widthFromDefaultWindowFraction(pathColumnWidthFraction);
            case columnType:
                return widthFromDefaultWindowFraction(typeColumnWidthFraction);
            case columnOverallPeak:
                return widthFromDefaultWindowFraction(overallPeakColumnWidthFraction);
            case columnPeakLeft:
                return widthFromDefaultWindowFraction(peakLeftColumnWidthFraction);
            case columnPeakRight:
                return widthFromDefaultWindowFraction(peakRightColumnWidthFraction);
            case columnStatus:
                return widthFromDefaultWindowFraction(statusColumnWidthFraction);
        }

        return 0;
    }

    static consteval int minimumColumnWidth(ColumnId columnId)
    {
        switch (columnId) {
            case columnName:
                return nameColumnMinimumWidth;
            case columnPath:
                return pathColumnMinimumWidth;
            case columnType:
                return typeColumnMinimumWidth;
            case columnOverallPeak:
            case columnPeakLeft:
            case columnPeakRight:
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
        std::function<void(int row, int columnId, const juce::MouseEvent& event)> contextMenuRequested
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

private:
    static constexpr int defaultWindowWidth = 1024;
    static constexpr int nameColumnMinimumWidth = 120;
    static constexpr int pathColumnMinimumWidth = 160;
    static constexpr int typeColumnMinimumWidth = 96;
    static constexpr int metricColumnMinimumWidth = 90;
    static constexpr double nameColumnWidthFraction = 220.0 / defaultWindowWidth;
    static constexpr double pathColumnWidthFraction = 280.0 / defaultWindowWidth;
    static constexpr double typeColumnWidthFraction = 110.0 / defaultWindowWidth;
    static constexpr double overallPeakColumnWidthFraction = 100.0 / defaultWindowWidth;
    static constexpr double peakLeftColumnWidthFraction = 100.0 / defaultWindowWidth;
    static constexpr double peakRightColumnWidthFraction = 100.0 / defaultWindowWidth;
    static constexpr double statusColumnWidthFraction = 114.0 / defaultWindowWidth;
    static constexpr double totalInitialColumnFraction = nameColumnWidthFraction + pathColumnWidthFraction
        + typeColumnWidthFraction + overallPeakColumnWidthFraction + peakLeftColumnWidthFraction
        + peakRightColumnWidthFraction + statusColumnWidthFraction;

    static_assert(totalInitialColumnFraction == 1.0, "Initial column fractions must sum to 1.0");

    static consteval int widthFromDefaultWindowFraction(double fraction)
    {
        return static_cast<int>(fraction * defaultWindowWidth);
    }

    std::vector<AudioAnalysisRecord>& records;
    std::function<void(int row)> selectionChanged;
    std::function<void(int columnId, bool isForwards)> sortChanged;
    std::function<void(int row, int columnId, const juce::MouseEvent& event)> contextMenuRequested;
};
