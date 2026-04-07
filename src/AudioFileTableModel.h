#pragma once

#include "AudioAnalysisService.h"

#include <JuceHeader.h>

#include <functional>
#include <vector>

class AudioFileTableModel : public juce::TableListBoxModel
{
public:
    enum ColumnId {
        columnName = 1,
        columnPath,
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
            case columnOverallPeak:
            case columnPeakLeft:
            case columnPeakRight:
            case columnStatus:
                return metricColumnMinimumWidth;
        }

        return 0;
    }

    AudioFileTableModel(
        std::vector<AudioAnalysisRecord>& records,
        std::function<void(int row)> selectionChanged,
        std::function<void(int columnId, bool isForwards)> sortChanged,
        std::function<void(int row, int columnId, const juce::MouseEvent& event)> contextMenuRequested
    );

    static void configureHeader(juce::TableHeaderComponent& header);

    int getNumRows() override;
    juce::var getDragSourceDescription(const juce::SparseSet<int>& currentlySelectedRows) override;
    void paintCell(juce::Graphics& g, int rowNumber, int columnId, int width, int height, bool rowIsSelected) override;
    void paintRowBackground(juce::Graphics& g, int rowNumber, int width, int height, bool rowIsSelected) override;
    void cellClicked(int rowNumber, int columnId, const juce::MouseEvent& event) override;
    void selectedRowsChanged(int lastRowSelected) override;
    void sortOrderChanged(int newSortColumnId, bool isForwards) override;

private:
    static constexpr int defaultWindowWidth = 1024;
    static constexpr int nameColumnMinimumWidth = 120;
    static constexpr int pathColumnMinimumWidth = 160;
    static constexpr int metricColumnMinimumWidth = 90;
    static constexpr double nameColumnWidthFraction = 244.0 / defaultWindowWidth;
    static constexpr double pathColumnWidthFraction = 380.0 / defaultWindowWidth;
    static constexpr double overallPeakColumnWidthFraction = 100.0 / defaultWindowWidth;
    static constexpr double peakLeftColumnWidthFraction = 100.0 / defaultWindowWidth;
    static constexpr double peakRightColumnWidthFraction = 100.0 / defaultWindowWidth;
    static constexpr double statusColumnWidthFraction = 100.0 / defaultWindowWidth;
    static constexpr double totalInitialColumnFraction = nameColumnWidthFraction + pathColumnWidthFraction
        + overallPeakColumnWidthFraction + peakLeftColumnWidthFraction + peakRightColumnWidthFraction
        + statusColumnWidthFraction;

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
