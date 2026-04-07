#include "AudioFileTableModel.h"

#include "CustomLookAndFeel.h"

namespace
{
juce::String getRecordTypeLabel(const AudioAnalysisRecord& record)
{
    auto extension = record.file.getFileExtension().trimCharactersAtStart(".");

    if (extension.isNotEmpty()) {
        return extension.toUpperCase();
    }

    auto formatName = record.formatName.trim();

    if (formatName.endsWithIgnoreCase(" file")) {
        formatName = formatName.dropLastCharacters(5).trimEnd();
    }

    if (formatName.isNotEmpty()) {
        return formatName;
    }

    return "Unknown";
}

void drawActivityIndicator(juce::Graphics& g, juce::Rectangle<float> bounds, float phase)
{
    const auto ringBounds = bounds.reduced(1.0f);
    const auto accent = juce::CustomLookAndFeel::blue.withAlpha(0.9f);
    const auto background = juce::CustomLookAndFeel::greyMiddle.withAlpha(0.3f);
    const auto startAngle = (phase * juce::MathConstants<float>::twoPi) - juce::MathConstants<float>::halfPi;
    const auto endAngle = startAngle + juce::MathConstants<float>::pi * 1.2f;

    g.setColour(background);
    g.fillEllipse(ringBounds);

    juce::Path arc;
    arc.addPieSegment(
        ringBounds.getX(), ringBounds.getY(), ringBounds.getWidth(), ringBounds.getHeight(), startAngle, endAngle, 0.52f
    );

    g.setColour(accent);
    g.fillPath(arc);

    g.setColour(juce::Colours::white.withAlpha(0.12f));
    g.drawEllipse(ringBounds, 1.0f);
}
}  // namespace

/// Builds the results table model used by the main file list.
AudioFileTableModel::AudioFileTableModel(
    std::vector<AudioAnalysisRecord>& recordsToUse,
    std::function<void(int row)> selectionChangedCallback,
    std::function<void(int columnId, bool isForwards)> sortChangedCallback,
    std::function<void(int row, int columnId, const juce::MouseEvent& event)> contextMenuRequestedCallback,
    std::function<juce::String(const AudioAnalysisRecord& record)> activeStatusLabelProviderCallback,
    std::function<float()> activityPhaseProviderCallback
) :
    records(recordsToUse),
    selectionChanged(std::move(selectionChangedCallback)),
    sortChanged(std::move(sortChangedCallback)),
    contextMenuRequested(std::move(contextMenuRequestedCallback)),
    activeStatusLabelProvider(std::move(activeStatusLabelProviderCallback)),
    activityPhaseProvider(std::move(activityPhaseProviderCallback))
{ }

void AudioFileTableModel::configureHeader(juce::TableHeaderComponent& header)
{
    header.addColumn(
        "Name",
        columnName,
        initialColumnWidth(columnName),
        minimumColumnWidth(columnName),
        600,
        juce::TableHeaderComponent::defaultFlags
    );
    header.addColumn(
        "Path",
        columnPath,
        initialColumnWidth(columnPath),
        minimumColumnWidth(columnPath),
        1200,
        juce::TableHeaderComponent::defaultFlags
    );
    header.addColumn(
        "Type",
        columnType,
        initialColumnWidth(columnType),
        minimumColumnWidth(columnType),
        160,
        juce::TableHeaderComponent::defaultFlags
    );
    header.addColumn(
        "Bitrate",
        columnBitrate,
        initialColumnWidth(columnBitrate),
        minimumColumnWidth(columnBitrate),
        160,
        juce::TableHeaderComponent::defaultFlags
    );
    header.addColumn(
        "Peak Max",
        columnOverallPeak,
        initialColumnWidth(columnOverallPeak),
        minimumColumnWidth(columnOverallPeak),
        160,
        juce::TableHeaderComponent::defaultFlags
    );
    header.addColumn(
        "Peak L",
        columnPeakLeft,
        initialColumnWidth(columnPeakLeft),
        minimumColumnWidth(columnPeakLeft),
        160,
        juce::TableHeaderComponent::defaultFlags
    );
    header.addColumn(
        "Peak R",
        columnPeakRight,
        initialColumnWidth(columnPeakRight),
        minimumColumnWidth(columnPeakRight),
        160,
        juce::TableHeaderComponent::defaultFlags
    );
    header.addColumn(
        "Status",
        columnStatus,
        initialColumnWidth(columnStatus),
        minimumColumnWidth(columnStatus),
        200,
        juce::TableHeaderComponent::defaultFlags
    );
}

int AudioFileTableModel::getNumRows()
{
    return static_cast<int>(records.size());
}

juce::var AudioFileTableModel::getDragSourceDescription(const juce::SparseSet<int>& currentlySelectedRows)
{
    juce::StringArray filePaths;

    for (int index = 0; index < currentlySelectedRows.size(); ++index) {
        const auto rowNumber = currentlySelectedRows[index];

        if (!juce::isPositiveAndBelow(rowNumber, getNumRows())) {
            continue;
        }

        const auto& record = records[static_cast<std::size_t>(rowNumber)];
        if (record.file.existsAsFile()) {
            filePaths.addIfNotAlreadyThere(record.file.getFullPathName());
        }
    }

    return filePaths.joinIntoString("\n");
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
    auto textBounds = juce::Rectangle<int>(4, 0, width - 8, height);

    switch (columnId) {
        case columnName:
            text = record.fileName;
            break;
        case columnPath:
            text = record.fullPath;
            break;
        case columnType:
            text = getRecordTypeLabel(record);
            break;
        case columnBitrate:
            text = AudioAnalysisService::formatBitrateDisplay(record);
            justification = juce::Justification::centredRight;
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
            text = activeStatusLabelProvider != nullptr ? activeStatusLabelProvider(record) : juce::String();

            if (text.isEmpty()) {
                text = AudioAnalysisService::formatStatus(record);
            } else {
                const auto indicatorSize = static_cast<float>(juce::jmin(height - 8, 14));
                const auto indicatorArea = juce::Rectangle<float>(
                    static_cast<float>(textBounds.getX()),
                    (static_cast<float>(height) - indicatorSize) * 0.5f,
                    indicatorSize,
                    indicatorSize
                );
                drawActivityIndicator(
                    g, indicatorArea, activityPhaseProvider != nullptr ? activityPhaseProvider() : 0.0f
                );
                textBounds.removeFromLeft(static_cast<int>(indicatorSize) + 8);
            }
            break;
        default:
            break;
    }

    g.setColour(juce::Colours::white.withAlpha(record.hasError() ? 0.72f : 0.95f));
    g.drawText(text, textBounds, justification, true);

    g.setColour(juce::Colours::white.withAlpha(0.08f));
    g.fillRect(width - 1, 0, 1, height);
}

void AudioFileTableModel::selectedRowsChanged(int lastRowSelected)
{
    if (selectionChanged != nullptr) {
        selectionChanged(lastRowSelected);
    }
}

void AudioFileTableModel::cellClicked(int rowNumber, int columnId, const juce::MouseEvent& event)
{
    if (event.mods.isPopupMenu() && contextMenuRequested != nullptr) {
        contextMenuRequested(rowNumber, columnId, event);
    }
}

void AudioFileTableModel::sortOrderChanged(int newSortColumnId, bool isForwards)
{
    if (sortChanged != nullptr) {
        sortChanged(newSortColumnId, isForwards);
    }
}

juce::String AudioFileTableModel::getCellTooltip(int rowNumber, int columnId)
{
    juce::ignoreUnused(rowNumber, columnId);
    return {};
}
