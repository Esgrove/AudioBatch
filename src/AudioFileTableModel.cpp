/// Implementation of the AudioFileTableModel results table.
/// Covers cell text and colour selection, row sorting,
/// the Type column label derivation,
/// and the animated activity indicator drawn in the Status column.

#include "AudioFileTableModel.h"

#include "CustomLookAndFeel.h"
#include "StringFormat.h"
#include <string_view>

namespace audiobatch::table
{
/// Suffix that some decoders append to their format name, for example "WAV file".
/// It is stripped when the format name is used as the Type column label.
constexpr auto formatNameFileSuffix = " file";

/// Maximum widths a user can drag each column to in the results table header.
/// The Name and Path columns may grow wide for long names,
/// while the metric columns are capped so a single column cannot crowd out the others.
namespace column_limits
{
/// Maximum width of the Name column in pixels.
constexpr int nameMaximumWidth = 600;
/// Maximum width of the Path column in pixels.
constexpr int pathMaximumWidth = 1200;
/// Shared maximum width of the Type, Bitrate, Rate, and metric columns in pixels.
constexpr int metricMaximumWidth = 160;
/// Maximum width of the Status column in pixels.
constexpr int statusMaximumWidth = 200;
}  // namespace column_limits

/// Inner radius of the activity indicator arc as a proportion of the outer radius,
/// which turns the pie segment into a ring.
constexpr float activityIndicatorInnerRadiusProportion = 0.52f;

/// Opacity of the white outline drawn around the activity indicator ring.
constexpr float activityIndicatorOutlineAlpha = 0.12f;

/// Opacity of the blue fill drawn behind selected rows.
constexpr float selectedRowHighlightAlpha = 0.22f;

/// Opacity of the cell text for rows with an analysis error, dimmed to set them apart.
constexpr float errorCellTextAlpha = 0.72f;

/// Opacity of the cell text for rows without errors.
constexpr float normalCellTextAlpha = 0.95f;

/// Opacity of the one pixel vertical divider drawn at the right edge of each cell.
constexpr float cellDividerAlpha = 0.08f;

/// Derives a short display label for the Type column.
/// Prefers the file extension in upper case,
/// then falls back to the decoder format name with a trailing " file" suffix stripped,
/// and finally to "Unknown".
static juce::String getRecordTypeLabel(const AudioAnalysisRecord& record)
{
    const auto extension = record.file.getFileExtension().trimCharactersAtStart(".");

    if (extension.isNotEmpty()) {
        return extension.toUpperCase();
    }

    auto formatName = record.formatName.trim();

    if (formatName.endsWithIgnoreCase(formatNameFileSuffix)) {
        formatName
            = formatName.dropLastCharacters(static_cast<int>(std::string_view {formatNameFileSuffix}.size())).trimEnd();
    }

    if (formatName.isNotEmpty()) {
        return formatName;
    }

    return "Unknown";
}

/// Draws the small spinning arc shown in the Status column while a row is being processed.
/// The phase argument in the range 0 to 1 controls the arc rotation,
/// so repeated repaints with an advancing phase animate the spinner.
static void drawActivityIndicator(juce::Graphics& graphics, const juce::Rectangle<float> bounds, const float phase)
{
    const auto ringBounds = bounds.reduced(1.0f);
    const auto accent = juce::CustomLookAndFeel::blue().withAlpha(0.9f);
    const auto background = juce::CustomLookAndFeel::greyMiddle().withAlpha(0.3f);
    const auto startAngle = phase * juce::MathConstants<float>::twoPi - juce::MathConstants<float>::halfPi;
    const auto endAngle = startAngle + juce::MathConstants<float>::pi * 1.2f;

    graphics.setColour(background);
    graphics.fillEllipse(ringBounds);

    juce::Path arc;
    arc.addPieSegment(
        ringBounds.getX(),
        ringBounds.getY(),
        ringBounds.getWidth(),
        ringBounds.getHeight(),
        startAngle,
        endAngle,
        activityIndicatorInnerRadiusProportion
    );

    graphics.setColour(accent);
    graphics.fillPath(arc);

    graphics.setColour(juce::Colours::white.withAlpha(activityIndicatorOutlineAlpha));
    graphics.drawEllipse(ringBounds, 1.0f);
}
}  // namespace audiobatch::table

using namespace audiobatch::table;

AudioFileTableModel::AudioFileTableModel(
    std::vector<AudioAnalysisRecord>& sourceRecords,
    std::function<void(int row)> onSelectionChanged,
    std::function<void(int columnId, bool isForwards)> onSortChanged,
    std::function<void(int row, int columnId, const juce::MouseEvent& event)> onContextMenuRequested,
    std::function<juce::String(const AudioAnalysisRecord& record)> statusLabelProvider,
    std::function<float()> phaseProvider
) :
    records(&sourceRecords),
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
        column_limits::nameMaximumWidth,
        juce::TableHeaderComponent::defaultFlags
    );
    header.addColumn(
        "Path",
        columnPath,
        initialColumnWidth(columnPath),
        minimumColumnWidth(columnPath),
        column_limits::pathMaximumWidth,
        juce::TableHeaderComponent::defaultFlags
    );
    header.addColumn(
        "Type",
        columnType,
        initialColumnWidth(columnType),
        minimumColumnWidth(columnType),
        column_limits::metricMaximumWidth,
        juce::TableHeaderComponent::defaultFlags
    );
    header.addColumn(
        "Bitrate",
        columnBitrate,
        initialColumnWidth(columnBitrate),
        minimumColumnWidth(columnBitrate),
        column_limits::metricMaximumWidth,
        juce::TableHeaderComponent::defaultFlags
    );
    header.addColumn(
        "Rate",
        columnSampleRate,
        initialColumnWidth(columnSampleRate),
        minimumColumnWidth(columnSampleRate),
        column_limits::metricMaximumWidth,
        juce::TableHeaderComponent::defaultFlags
    );
    header.addColumn(
        "Peak",
        columnOverallPeak,
        initialColumnWidth(columnOverallPeak),
        minimumColumnWidth(columnOverallPeak),
        column_limits::metricMaximumWidth,
        juce::TableHeaderComponent::defaultFlags
    );
    header.addColumn(
        "Peak L",
        columnPeakLeft,
        initialColumnWidth(columnPeakLeft),
        minimumColumnWidth(columnPeakLeft),
        column_limits::metricMaximumWidth,
        juce::TableHeaderComponent::defaultFlags
    );
    header.addColumn(
        "Peak R",
        columnPeakRight,
        initialColumnWidth(columnPeakRight),
        minimumColumnWidth(columnPeakRight),
        column_limits::metricMaximumWidth,
        juce::TableHeaderComponent::defaultFlags
    );
    header.addColumn(
        "True Peak",
        columnOverallTruePeak,
        initialColumnWidth(columnOverallTruePeak),
        minimumColumnWidth(columnOverallTruePeak),
        column_limits::metricMaximumWidth,
        juce::TableHeaderComponent::defaultFlags
    );
    header.addColumn(
        "LUFS-S",
        columnMaxShortTermLufs,
        initialColumnWidth(columnMaxShortTermLufs),
        minimumColumnWidth(columnMaxShortTermLufs),
        column_limits::metricMaximumWidth,
        juce::TableHeaderComponent::defaultFlags
    );
    header.addColumn(
        "LUFS-I",
        columnIntegratedLufs,
        initialColumnWidth(columnIntegratedLufs),
        minimumColumnWidth(columnIntegratedLufs),
        column_limits::metricMaximumWidth,
        juce::TableHeaderComponent::defaultFlags
    );
    header.addColumn(
        "Gain",
        columnCustomGain,
        initialColumnWidth(columnCustomGain),
        minimumColumnWidth(columnCustomGain),
        column_limits::metricMaximumWidth,
        juce::TableHeaderComponent::defaultFlags
    );
    header.addColumn(
        "Status",
        columnStatus,
        initialColumnWidth(columnStatus),
        minimumColumnWidth(columnStatus),
        column_limits::statusMaximumWidth,
        juce::TableHeaderComponent::defaultFlags
    );

    header.setColumnVisible(columnPeakLeft, false);
    header.setColumnVisible(columnPeakRight, false);
    header.setColumnVisible(columnCustomGain, false);
}

int AudioFileTableModel::getNumRows()
{
    return static_cast<int>(records->size());
}

juce::var AudioFileTableModel::getDragSourceDescription(const juce::SparseSet<int>& currentlySelectedRows)
{
    juce::StringArray filePaths;

    for (int index = 0; index < currentlySelectedRows.size(); ++index) {
        const auto rowNumber = currentlySelectedRows[index];

        if (!juce::isPositiveAndBelow(rowNumber, getNumRows())) {
            continue;
        }

        if (const auto& record = (*records)[static_cast<std::size_t>(rowNumber)]; record.file.existsAsFile()) {
            filePaths.addIfNotAlreadyThere(record.file.getFullPathName());
        }
    }

    return filePaths.joinIntoString("\n");
}

void AudioFileTableModel::paintRowBackground(
    juce::Graphics& graphics,
    int rowNumber,
    int width,
    int height,
    const bool rowIsSelected
)
{
    juce::ignoreUnused(rowNumber, width, height);

    if (rowIsSelected) {
        graphics.fillAll(juce::CustomLookAndFeel::blue().withAlpha(selectedRowHighlightAlpha));
        return;
    }

    graphics.fillAll(
        rowNumber % 2 == 0 ? juce::CustomLookAndFeel::greySemiDark() : juce::CustomLookAndFeel::greyMediumDark()
    );
}

void AudioFileTableModel::paintCell(
    juce::Graphics& graphics,
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

    const auto& record = (*records)[static_cast<std::size_t>(rowNumber)];
    juce::String text;
    auto justification = juce::Justification::centredLeft;
    auto textBounds = juce::Rectangle(4, 0, width - 8, height);
    auto font = juce::CustomLookAndFeel::getTextFont();

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
        case columnCustomGain:
            if (record.hasCustomGain) {
                text = utils::format("{}{:.1f} dB", record.customGainDb >= 0.0f ? "+" : "", record.customGainDb);
            } else {
                text = {};
            }
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
                    graphics, indicatorArea, activityPhaseProvider != nullptr ? activityPhaseProvider() : 0.0f
                );
                textBounds.removeFromLeft(static_cast<int>(indicatorSize) + 8);
            }
            break;
        default:
            break;
    }

    graphics.setColour(juce::Colours::white.withAlpha(record.hasError() ? errorCellTextAlpha : normalCellTextAlpha));
    graphics.setFont(font);
    graphics.drawText(text, textBounds, justification, true);

    graphics.setColour(juce::Colours::white.withAlpha(cellDividerAlpha));
    graphics.fillRect(width - 1, 0, 1, height);
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
