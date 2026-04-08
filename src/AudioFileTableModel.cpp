#include "AudioFileTableModel.h"

#include "CustomLookAndFeel.h"

namespace audiobatch::table
{
static juce::String getRecordTypeLabel(const AudioAnalysisRecord& record)
{
    const auto extension = record.file.getFileExtension().trimCharactersAtStart(".");

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

static void drawActivityIndicator(juce::Graphics& g, const juce::Rectangle<float> bounds, const float phase)
{
    const auto ringBounds = bounds.reduced(1.0f);
    const auto accent = juce::CustomLookAndFeel::blue.withAlpha(0.9f);
    const auto background = juce::CustomLookAndFeel::greyMiddle.withAlpha(0.3f);
    const auto startAngle = phase * juce::MathConstants<float>::twoPi - juce::MathConstants<float>::halfPi;
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
}  // namespace audiobatch::table

using namespace audiobatch::table;

/// Builds the results table model used by the main file list.
AudioFileTableModel::AudioFileTableModel(
    std::vector<AudioAnalysisRecord>& sourceRecords,
    std::function<void(int row)> onSelectionChanged,
    std::function<void(int columnId, bool isForwards)> onSortChanged,
    std::function<void(int row, int columnId, const juce::MouseEvent& event)> onContextMenuRequested,
    std::function<juce::String(const AudioAnalysisRecord& record)> statusLabelProvider,
    std::function<float()> phaseProvider
) :
    records(sourceRecords),
    selectionChanged(std::move(onSelectionChanged)),
    sortChanged(std::move(onSortChanged)),
    contextMenuRequested(std::move(onContextMenuRequested)),
    activeStatusLabelProvider(std::move(statusLabelProvider)),
    activityPhaseProvider(std::move(phaseProvider))
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
        "Rate",
        columnSampleRate,
        initialColumnWidth(columnSampleRate),
        minimumColumnWidth(columnSampleRate),
        160,
        juce::TableHeaderComponent::defaultFlags
    );
    header.addColumn(
        "Peak",
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
        "True Peak",
        columnOverallTruePeak,
        initialColumnWidth(columnOverallTruePeak),
        minimumColumnWidth(columnOverallTruePeak),
        160,
        juce::TableHeaderComponent::defaultFlags
    );
    header.addColumn(
        "LUFS-S",
        columnMaxShortTermLufs,
        initialColumnWidth(columnMaxShortTermLufs),
        minimumColumnWidth(columnMaxShortTermLufs),
        160,
        juce::TableHeaderComponent::defaultFlags
    );
    header.addColumn(
        "LUFS-I",
        columnIntegratedLufs,
        initialColumnWidth(columnIntegratedLufs),
        minimumColumnWidth(columnIntegratedLufs),
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

    header.setColumnVisible(columnPeakLeft, false);
    header.setColumnVisible(columnPeakRight, false);
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

        if (const auto& record = records[static_cast<std::size_t>(rowNumber)]; record.file.existsAsFile()) {
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
    const bool rowIsSelected
)
{
    juce::ignoreUnused(rowNumber, width, height);

    if (rowIsSelected) {
        g.fillAll(juce::CustomLookAndFeel::blue.withAlpha(0.22f));
        return;
    }

    g.fillAll(rowNumber % 2 == 0 ? juce::CustomLookAndFeel::greySemiDark : juce::CustomLookAndFeel::greyMediumDark);
}

void AudioFileTableModel::paintCell(
    juce::Graphics& g,
    const int rowNumber,
    const int columnId,
    const int width,
    const int height,
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
    auto textBounds = juce::Rectangle(4, 0, width - 8, height);
    auto font = juce::CustomLookAndFeel::textFont;

    switch (columnId) {
        case columnName:
            text = record.fileName;
            break;
        case columnPath:
            text = record.fullPath;
            break;
        case columnType:
            text = getRecordTypeLabel(record);
            font = juce::CustomLookAndFeel::get_mono_font();
            break;
        case columnBitrate:
            text = AudioAnalysisService::formatBitrateDisplay(record);
            justification = juce::Justification::centredRight;
            break;
        case columnSampleRate:
            text = AudioAnalysisService::formatSampleRateDisplay(record);
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
        case columnOverallTruePeak:
            text = AudioAnalysisService::formatTruePeakDisplay(record.overallTruePeak);
            justification = juce::Justification::centredRight;
            break;
        case columnMaxShortTermLufs:
            text = AudioAnalysisService::formatLoudnessDisplay(record.maxShortTermLufs);
            justification = juce::Justification::centredRight;
            break;
        case columnIntegratedLufs:
            text = AudioAnalysisService::formatLoudnessDisplay(record.integratedLufs);
            justification = juce::Justification::centredRight;
            break;
        case columnStatus:
            text = activeStatusLabelProvider != nullptr ? activeStatusLabelProvider(record) : juce::String();

            if (text.isEmpty()) {
                text = AudioAnalysisService::formatStatus(record);
            } else {
                const auto indicatorSize = static_cast<float>(juce::jmin(height - 8, 14));
                const auto indicatorArea = juce::Rectangle(
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
    g.setFont(font);
    g.drawText(text, textBounds, justification, true);

    g.setColour(juce::Colours::white.withAlpha(0.08f));
    g.fillRect(width - 1, 0, 1, height);
}

void AudioFileTableModel::selectedRowsChanged(const int lastRowSelected)
{
    if (selectionChanged != nullptr) {
        selectionChanged(lastRowSelected);
    }
}

void AudioFileTableModel::cellClicked(const int rowNumber, const int columnId, const juce::MouseEvent& event)
{
    if (event.mods.isPopupMenu() && contextMenuRequested != nullptr) {
        contextMenuRequested(rowNumber, columnId, event);
    }
}

void AudioFileTableModel::sortOrderChanged(const int newSortColumnId, const bool isForwards)
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
