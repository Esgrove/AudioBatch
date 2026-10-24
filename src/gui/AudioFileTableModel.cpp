/// Implementation of the AudioFileTableModel results table.
/// Covers cell text and colour selection, row sorting, the Type column label derivation,
/// the activity labels, and the waiting, progress, and spinning activity indicator drawn in the Status column.

#include "gui/AudioFileTableModel.h"

#include "analysis/AudioAnalysisFormatting.h"
#include "gui/components/CustomLookAndFeel.h"
#include "gui/FileActivityTracker.h"
#include "utils/StringFormat.h"

namespace audiobatch
{
namespace
{
/// Inner radius of the activity indicator arc as a proportion of the outer radius,
/// which turns the pie segment into a ring.
constexpr float activityIndicatorInnerRadiusProportion {0.52f};

/// Opacity of the white outline drawn around the activity indicator ring.
constexpr float activityIndicatorOutlineAlpha {0.12f};

/// Opacity of the blue fill drawn behind selected rows.
constexpr float selectedRowHighlightAlpha {0.22f};

/// Opacity of the cell text for rows with an analysis error, dimmed to set them apart.
constexpr float errorCellTextAlpha {0.72f};

/// Opacity of the cell text for rows without errors.
constexpr float normalCellTextAlpha {0.95f};

/// Opacity of the one pixel vertical divider drawn at the right edge of each cell.
constexpr float cellDividerAlpha {0.08f};

/// Opacity of the status text of a waiting row, dimmed to set it apart from the rows being worked on.
constexpr float waitingStatusTextAlpha {0.55f};

/// Smallest progress drawn as a filled arc, so a just started file does not show a sliver.
constexpr float minimumDrawnProgress {0.005f};

/// Length of the spinning arc shown while a busy file's progress is unknown, as a fraction of a full turn.
constexpr float spinnerArcTurns {0.6f};

/// Draws the small activity indicator shown in the Status column while a row is busy.
/// A waiting row gets an empty ring.
/// A row with known progress gets a ring that fills clockwise from the top like a progress bar.
/// A row whose progress is still unknown gets a spinning arc,
/// where the phase argument in the range 0 to 1 controls the rotation,
/// so repeated repaints with an advancing phase animate the spinner.
void drawActivityIndicator(
    juce::Graphics& graphics,
    const juce::Rectangle<float> bounds,
    const FileActivityState& state,
    const float phase
)
{
    const auto ringBounds = bounds.reduced(1.0f);
    const auto accent = juce::CustomLookAndFeel::blue().withAlpha(0.9f);
    const auto background = juce::CustomLookAndFeel::greyMiddle().withAlpha(0.3f);

    graphics.setColour(background);
    graphics.fillEllipse(ringBounds);

    auto startAngle = 0.0f;
    auto endAngle = 0.0f;

    // A waiting file keeps both angles at zero, which leaves the ring empty.
    if (state.activity != FileActivity::waiting && state.progress.has_value()) {
        endAngle = *state.progress >= minimumDrawnProgress ? *state.progress * juce::MathConstants<float>::twoPi : 0.0f;
    } else if (state.activity != FileActivity::waiting) {
        startAngle = phase * juce::MathConstants<float>::twoPi;
        endAngle = startAngle + juce::MathConstants<float>::twoPi * spinnerArcTurns;
    }

    if (endAngle > startAngle) {
        juce::Path arc;
        arc.addPieSegment(ringBounds, startAngle, endAngle, activityIndicatorInnerRadiusProportion);

        graphics.setColour(accent);
        graphics.fillPath(arc);
    }

    graphics.setColour(juce::Colours::white.withAlpha(activityIndicatorOutlineAlpha));
    graphics.drawEllipse(ringBounds, 1.0f);
}
}  // namespace

/// Creates a model backed by the live result list and UI callbacks owned by the main view.
/// The operation message provider returns the outcome of the last normalize or process run for a record,
/// or an empty message, and is shown in the status column when the file is idle.
AudioFileTableModel::AudioFileTableModel(
    const std::vector<AudioAnalysisRecord>& sourceRecords,
    std::function<void(int row)> onSelectionChanged,
    std::function<void(int columnId, bool isForwards)> onSortChanged,
    std::function<void(int row, int columnId, const juce::MouseEvent& event)> onContextMenuRequested,
    std::function<std::optional<FileActivityState>(const AudioAnalysisRecord& record)> activityProvider,
    std::function<OperationMessage(const AudioAnalysisRecord& record)> messageProvider,
    std::function<float()> phaseProvider
) :
    records(&sourceRecords),
    selectionChanged(std::move(onSelectionChanged)),
    sortChanged(std::move(onSortChanged)),
    contextMenuRequested(std::move(onContextMenuRequested)),
    activityStateProvider(std::move(activityProvider)),
    operationMessageProvider(std::move(messageProvider)),
    activityPhaseProvider(std::move(phaseProvider))
{}

/// Installs the standard set of columns used by the results table.
void AudioFileTableModel::configureHeader(juce::TableHeaderComponent& header)
{
    header.addColumn(
        "Name",
        columnName,
        layout::table::initialColumnWidth(columnName),
        layout::table::minimumColumnWidth(columnName),
        layout::table::nameMaximumWidth,
        juce::TableHeaderComponent::defaultFlags
    );
    header.addColumn(
        "Path",
        columnPath,
        layout::table::initialColumnWidth(columnPath),
        layout::table::minimumColumnWidth(columnPath),
        layout::table::pathMaximumWidth,
        juce::TableHeaderComponent::defaultFlags
    );
    header.addColumn(
        "Type",
        columnType,
        layout::table::initialColumnWidth(columnType),
        layout::table::minimumColumnWidth(columnType),
        layout::table::metricMaximumWidth,
        juce::TableHeaderComponent::defaultFlags
    );
    header.addColumn(
        "Bit Depth",
        columnBitDepth,
        layout::table::initialColumnWidth(columnBitDepth),
        layout::table::minimumColumnWidth(columnBitDepth),
        layout::table::metricMaximumWidth,
        juce::TableHeaderComponent::defaultFlags
    );
    header.addColumn(
        "Bitrate",
        columnBitrate,
        layout::table::initialColumnWidth(columnBitrate),
        layout::table::minimumColumnWidth(columnBitrate),
        layout::table::metricMaximumWidth,
        juce::TableHeaderComponent::defaultFlags
    );
    header.addColumn(
        "Rate",
        columnSampleRate,
        layout::table::initialColumnWidth(columnSampleRate),
        layout::table::minimumColumnWidth(columnSampleRate),
        layout::table::metricMaximumWidth,
        juce::TableHeaderComponent::defaultFlags
    );
    header.addColumn(
        "Peak",
        columnOverallPeak,
        layout::table::initialColumnWidth(columnOverallPeak),
        layout::table::minimumColumnWidth(columnOverallPeak),
        layout::table::metricMaximumWidth,
        juce::TableHeaderComponent::defaultFlags
    );
    header.addColumn(
        "Peak L",
        columnPeakLeft,
        layout::table::initialColumnWidth(columnPeakLeft),
        layout::table::minimumColumnWidth(columnPeakLeft),
        layout::table::metricMaximumWidth,
        juce::TableHeaderComponent::defaultFlags
    );
    header.addColumn(
        "Peak R",
        columnPeakRight,
        layout::table::initialColumnWidth(columnPeakRight),
        layout::table::minimumColumnWidth(columnPeakRight),
        layout::table::metricMaximumWidth,
        juce::TableHeaderComponent::defaultFlags
    );
    header.addColumn(
        "True Peak",
        columnOverallTruePeak,
        layout::table::initialColumnWidth(columnOverallTruePeak),
        layout::table::minimumColumnWidth(columnOverallTruePeak),
        layout::table::metricMaximumWidth,
        juce::TableHeaderComponent::defaultFlags
    );
    header.addColumn(
        "LUFS-S",
        columnMaxShortTermLufs,
        layout::table::initialColumnWidth(columnMaxShortTermLufs),
        layout::table::minimumColumnWidth(columnMaxShortTermLufs),
        layout::table::metricMaximumWidth,
        juce::TableHeaderComponent::defaultFlags
    );
    header.addColumn(
        "LUFS-I",
        columnIntegratedLufs,
        layout::table::initialColumnWidth(columnIntegratedLufs),
        layout::table::minimumColumnWidth(columnIntegratedLufs),
        layout::table::metricMaximumWidth,
        juce::TableHeaderComponent::defaultFlags
    );
    header.addColumn(
        "Gain",
        columnCustomGain,
        layout::table::initialColumnWidth(columnCustomGain),
        layout::table::minimumColumnWidth(columnCustomGain),
        layout::table::metricMaximumWidth,
        juce::TableHeaderComponent::defaultFlags
    );
    header.addColumn(
        "Status",
        columnStatus,
        layout::table::initialColumnWidth(columnStatus),
        layout::table::minimumColumnWidth(columnStatus),
        layout::table::statusMaximumWidth,
        juce::TableHeaderComponent::defaultFlags
    );

    header.setColumnVisible(columnPeakLeft, false);
    header.setColumnVisible(columnPeakRight, false);
    header.setColumnVisible(columnCustomGain, false);
}

/// Returns the number of analyzed rows currently available.
int AudioFileTableModel::getNumRows()
{
    return static_cast<int>(records->size());
}

/// Packages the selected file paths for external drag-and-drop.
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

/// Paints alternating row backgrounds and selection highlights.
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

/// Paints the text content for a single table cell.
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
    OperationMessage operationMessage;
    std::optional<FileActivityState> activityState;
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
            text = formatTypeLabel(record);
            font = juce::CustomLookAndFeel::get_mono_font();
            break;
        case columnBitDepth:
            text = formatBitsPerSampleDisplay(record);
            justification = juce::Justification::centredRight;
            break;
        case columnBitrate:
            text = formatBitrateDisplay(record);
            justification = juce::Justification::centredRight;
            break;
        case columnSampleRate:
            text = formatSampleRateDisplay(record);
            justification = juce::Justification::centredRight;
            break;
        case columnPeakLeft:
            text = formatPeakDisplay(record.peakLeft);
            justification = juce::Justification::centredRight;
            break;
        case columnPeakRight:
            text = formatPeakDisplay(record.peakRight);
            justification = juce::Justification::centredRight;
            break;
        case columnOverallPeak:
            text = formatPeakDisplay(record.overallPeak);
            justification = juce::Justification::centredRight;
            break;
        case columnOverallTruePeak:
            text = formatTruePeakDisplay(record.overallTruePeak);
            justification = juce::Justification::centredRight;
            break;
        case columnMaxShortTermLufs:
            text = formatLoudnessDisplay(record.maxShortTermLufs);
            justification = juce::Justification::centredRight;
            break;
        case columnIntegratedLufs:
            text = formatLoudnessDisplay(record.integratedLufs);
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
            activityState = activityStateProvider != nullptr ? activityStateProvider(record) : std::nullopt;

            if (!activityState.has_value()) {
                operationMessage = getOperationMessage(record);
                text = operationMessage.text.isNotEmpty() ? operationMessage.text : formatStatus(record);
            } else {
                text = getFileActivityLabel(activityState->activity);
                const auto indicatorSize = static_cast<float>(juce::jmin(height - 8, 14));
                const auto indicatorArea = juce::Rectangle(
                    static_cast<float>(textBounds.getX()),
                    (static_cast<float>(height) - indicatorSize) * 0.5f,
                    indicatorSize,
                    indicatorSize
                );
                drawActivityIndicator(
                    graphics,
                    indicatorArea,
                    *activityState,
                    activityPhaseProvider != nullptr ? activityPhaseProvider() : 0.0f
                );
                textBounds.removeFromLeft(static_cast<int>(indicatorSize) + 8);
            }
            break;
        default:
            break;
    }

    if (operationMessage.isError) {
        graphics.setColour(juce::Colours::orangered);
    } else if (activityState.has_value() && activityState->activity == FileActivity::waiting) {
        graphics.setColour(juce::Colours::white.withAlpha(waitingStatusTextAlpha));
    } else {
        graphics.setColour(
            juce::Colours::white.withAlpha(record.hasError() ? errorCellTextAlpha : normalCellTextAlpha)
        );
    }
    graphics.setFont(font);
    graphics.drawText(text, textBounds, justification, true);

    graphics.setColour(juce::Colours::white.withAlpha(cellDividerAlpha));
    graphics.fillRect(width - 1, 0, 1, height);
}

/// Notifies the owning component when the row selection changes.
void AudioFileTableModel::selectedRowsChanged(const int lastRowSelected)
{
    if (selectionChanged != nullptr) {
        selectionChanged(lastRowSelected);
    }
}

/// Forwards popup-clicks so the owning component can show file actions.
void AudioFileTableModel::cellClicked(const int rowNumber, const int columnId, const juce::MouseEvent& event)
{
    if (event.mods.isPopupMenu() && contextMenuRequested != nullptr) {
        contextMenuRequested(rowNumber, columnId, event);
    }
}

/// Notifies the owning component when the active sort column changes.
void AudioFileTableModel::sortOrderChanged(const int newSortColumnId, const bool isForwards)
{
    if (sortChanged != nullptr) {
        sortChanged(newSortColumnId, isForwards);
    }
}

/// Shows the full status text for the status column of a file with an error or notice, and no tooltip for other cells.
juce::String AudioFileTableModel::getCellTooltip(const int rowNumber, const int columnId)
{
    if (columnId != columnStatus || !juce::isPositiveAndBelow(rowNumber, static_cast<int>(records->size()))) {
        return {};
    }

    const auto& record = (*records)[static_cast<std::size_t>(rowNumber)];

    if (const auto [text, isError] = getOperationMessage(record); text.isNotEmpty()) {
        return text;
    }

    return record.hasError() ? record.errorMessage : juce::String();
}

/// Returns the last normalize or process message for a record from the provider, or an empty message.
OperationMessage AudioFileTableModel::getOperationMessage(const AudioAnalysisRecord& record) const
{
    return operationMessageProvider != nullptr ? operationMessageProvider(record) : OperationMessage {};
}
}  // namespace audiobatch
