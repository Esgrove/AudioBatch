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

    AudioFileTableModel(
        std::vector<AudioAnalysisRecord>& records,
        std::function<void(int row)> selectionChanged,
        std::function<void(int columnId, bool isForwards)> sortChanged
    );

    static void configureHeader(juce::TableHeaderComponent& header);

    int getNumRows() override;
    juce::var getDragSourceDescription(const juce::SparseSet<int>& currentlySelectedRows) override;
    void paintCell(juce::Graphics& g, int rowNumber, int columnId, int width, int height, bool rowIsSelected) override;
    void paintRowBackground(juce::Graphics& g, int rowNumber, int width, int height, bool rowIsSelected) override;
    void selectedRowsChanged(int lastRowSelected) override;
    void sortOrderChanged(int newSortColumnId, bool isForwards) override;

private:
    std::vector<AudioAnalysisRecord>& records;
    std::function<void(int row)> selectionChanged;
    std::function<void(int columnId, bool isForwards)> sortChanged;
};