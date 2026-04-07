#include "AudioFileTableModel.h"

#include "CustomLookAndFeel.h"

AudioFileTableModel::AudioFileTableModel(
    std::vector<AudioAnalysisRecord>& recordsToUse,
    std::function<void(int row)> selectionChangedCallback,
    std::function<void(int columnId, bool isForwards)> sortChangedCallback
) :
    records(recordsToUse),
    selectionChanged(std::move(selectionChangedCallback)),
    sortChanged(std::move(sortChangedCallback))
{ }

void AudioFileTableModel::configureHeader(juce::TableHeaderComponent& header)
{
    header.addColumn("Name", columnName, 220, 120, 600, juce::TableHeaderComponent::defaultFlags);
    header.addColumn("Path", columnPath, 360, 160, 1200, juce::TableHeaderComponent::defaultFlags);
    header.addColumn("Peak Max", columnOverallPeak, 120, 90, 160, juce::TableHeaderComponent::defaultFlags);
    header.addColumn("Peak L", columnPeakLeft, 120, 90, 160, juce::TableHeaderComponent::defaultFlags);
    header.addColumn("Peak R", columnPeakRight, 120, 90, 160, juce::TableHeaderComponent::defaultFlags);
    header.addColumn("Status", columnStatus, 120, 90, 200, juce::TableHeaderComponent::defaultFlags);
}

int AudioFileTableModel::getNumRows()
{
    return static_cast<int>(records.size());
}

void AudioFileTableModel::paintRowBackground(
    juce::Graphics& g,
    int rowNumber,
    int width,
    int height,
    bool rowIsSelected
)
{
    juce::ignoreUnused(rowNumber, width, height);

    if (rowIsSelected) {
        g.fillAll(juce::CustomLookAndFeel::blue.withAlpha(0.22f));
        return;
    }

    g.fillAll((rowNumber % 2 == 0) ? juce::CustomLookAndFeel::greySemiDark : juce::CustomLookAndFeel::greyMediumDark);
}

void AudioFileTableModel::paintCell(
    juce::Graphics& g,
    int rowNumber,
    int columnId,
    int width,
    int height,
    bool rowIsSelected
)
{
    juce::ignoreUnused(rowIsSelected);

    if (!juce::isPositiveAndBelow(rowNumber, getNumRows())) {
        return;
    }

    const auto& record = records[static_cast<std::size_t>(rowNumber)];
    juce::String text;
    auto justification = juce::Justification::centredLeft;

    switch (columnId) {
        case columnName:
            text = record.fileName;
            break;
        case columnPath:
            text = record.fullPath;
            break;
        case columnPeakLeft:
            text = AudioAnalysisService::formatPeakDisplay(record.peakLeft);
            justification = juce::Justification::centredRight;
            break;
        case columnPeakRight:
            text = AudioAnalysisService::formatPeakDisplay(record.peakRight);
            justification = juce::Justification::centredRight;
            break;
        case columnOverallPeak:
            text = AudioAnalysisService::formatPeakDisplay(record.overallPeak);
            justification = juce::Justification::centredRight;
            break;
        case columnStatus:
            text = AudioAnalysisService::formatStatus(record);
            break;
        default:
            break;
    }

    g.setColour(juce::Colours::white.withAlpha(record.hasError() ? 0.72f : 0.95f));
    g.drawText(text, juce::Rectangle<int>(4, 0, width - 8, height), justification, true);

    g.setColour(juce::Colours::white.withAlpha(0.08f));
    g.fillRect(width - 1, 0, 1, height);
}

void AudioFileTableModel::selectedRowsChanged(int lastRowSelected)
{
    if (selectionChanged != nullptr) {
        selectionChanged(lastRowSelected);
    }
}

void AudioFileTableModel::sortOrderChanged(int newSortColumnId, bool isForwards)
{
    if (sortChanged != nullptr) {
        sortChanged(newSortColumnId, isForwards);
    }
}