/// Implementation of the AudioFileTableModel results table.
/// Covers cell text and colour selection, row sorting, the Type column label derivation,
/// the activity labels, and the waiting, progress, and spinning activity indicator drawn in the Status column.

#include "gui/AudioFileTableModel.h"

#include "analysis/AudioAnalysisFormatting.h"
#include "gui/components/CustomLookAndFeel.h"
#include "gui/FileActivityTracker.h"
#include "utils/StringFormat.h"

#include <array>
#include <utility>

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

/// A results table column as the header registers it.
struct ColumnDefinition {
    /// The column, whose value is also its JUCE column id.
    AudioFileTableColumn column;
    /// Short title shown in the header.
    const char* title;
    /// What the column shows with its unit, for the header tooltip.
    const char* description;
    /// Width of the column when the table is first shown.
    int initialWidth;
    /// Narrowest width the user can drag the column to.
    int minimumWidth;
    /// Widest width the user can drag the column to.
    int maximumWidth;
    /// True when the column is shown before the user changes the visible columns.
    bool initiallyVisible;
};

/// Builds the definition of a column with its initial and minimum widths from GuiLayout.h.
consteval ColumnDefinition defineColumn(
    const AudioFileTableColumn column,
    const char* title,
    const char* description,
    const int maximumWidth,
    const bool initiallyVisible = true
)
{
    return {
        .column = column,
        .title = title,
        .description = description,
        .initialWidth = layout::table::initialColumnWidth(column),
        .minimumWidth = layout::table::minimumColumnWidth(column),
        .maximumWidth = maximumWidth,
        .initiallyVisible = initiallyVisible,
    };
}

/// The results table columns in their initial display order.
/// The short titles leave the units to the header tooltips, so the numeric columns stay narrow.
constexpr std::array columnDefinitions {
    defineColumn(AudioFileTableColumn::name, "Name", "File name", layout::table::nameMaximumWidth),
    defineColumn(AudioFileTableColumn::path, "Path", "Full path of the file", layout::table::pathMaximumWidth),
    defineColumn(AudioFileTableColumn::type, "Type", "File type", layout::table::metricMaximumWidth),
    defineColumn(
        AudioFileTableColumn::bitDepth,
        "Bit Depth",
        "Bit depth of the source audio",
        layout::table::metricMaximumWidth
    ),
    defineColumn(
        AudioFileTableColumn::bitrate,
        "Bitrate",
        "Average bitrate in kbps",
        layout::table::metricMaximumWidth
    ),
    defineColumn(AudioFileTableColumn::sampleRate, "Rate", "Sample rate in kHz", layout::table::metricMaximumWidth),
    defineColumn(
        AudioFileTableColumn::length,
        "Length",
        "Duration in minutes and seconds",
        layout::table::metricMaximumWidth
    ),
    defineColumn(AudioFileTableColumn::overallPeak, "Peak", "Sample peak in dBFS", layout::table::metricMaximumWidth),
    defineColumn(
        AudioFileTableColumn::peakLeft,
        "Peak L",
        "Sample peak of the left channel in dBFS",
        layout::table::metricMaximumWidth,
        false
    ),
    defineColumn(
        AudioFileTableColumn::peakRight,
        "Peak R",
        "Sample peak of the right channel in dBFS",
        layout::table::metricMaximumWidth,
        false
    ),
    defineColumn(
        AudioFileTableColumn::overallTruePeak,
        "True Peak",
        "True peak in dBTP",
        layout::table::metricMaximumWidth
    ),
    defineColumn(
        AudioFileTableColumn::maxShortTermLufs,
        "LUFS-S",
        "Maximum short-term loudness in LUFS",
        layout::table::metricMaximumWidth
    ),
    defineColumn(
        AudioFileTableColumn::integratedLufs,
        "LUFS-I",
        "Integrated loudness in LUFS",
        layout::table::metricMaximumWidth
    ),
    defineColumn(
        AudioFileTableColumn::customGain,
        "Gain",
        "Per-file gain in dB",
        layout::table::metricMaximumWidth,
        false
    ),
    defineColumn(AudioFileTableColumn::status, "Status", "Analysis status", layout::table::statusMaximumWidth),
};

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
    const auto accent = CustomLookAndFeel::blue().withAlpha(0.9f);
    const auto background = CustomLookAndFeel::greyMiddle().withAlpha(0.3f);

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
    const ResultsModel& sourceRecords,
    std::function<void(int row)> onSelectionChanged,
    std::function<void(std::optional<AudioFileTableColumn> column, bool isForwards)> onSortChanged,
    std::function<void(int row, const juce::MouseEvent& event)> onContextMenuRequested,
    std::function<std::optional<FileActivityState>(const AudioAnalysisRecord& record)> activityProvider,
    std::function<OperationMessage(const AudioAnalysisRecord& record)> messageProvider,
    std::function<float()> phaseProvider
) :
    records(sourceRecords),
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
    for (const auto& definition : columnDefinitions) {
        const auto columnId = std::to_underlying(definition.column);
        header.addColumn(
            definition.title,
            columnId,
            definition.initialWidth,
            definition.minimumWidth,
            definition.maximumWidth,
            juce::TableHeaderComponent::defaultFlags
        );
        if (!definition.initiallyVisible) {
            header.setColumnVisible(columnId, false);
        }
    }
}

/// Returns what a column shows with its unit, for the header tooltip, or an empty string for an unknown column.
juce::String AudioFileTableModel::getColumnDescription(const AudioFileTableColumn column)
{
    for (const auto& definition : columnDefinitions) {
        if (definition.column == column) {
            return definition.description;
        }
    }

    return {};
}

/// Returns the number of analyzed rows currently available.
int AudioFileTableModel::getNumRows()
{
    return records.size();
}

/// Packages the selected file paths for external drag-and-drop.
juce::var AudioFileTableModel::getDragSourceDescription(const juce::SparseSet<int>& currentlySelectedRows)
{
    juce::StringArray filePaths;

    for (int index = 0; index < currentlySelectedRows.size(); ++index) {
        const auto rowNumber = currentlySelectedRows[index];

        if (!records.containsRow(rowNumber)) {
            continue;
        }

        if (const auto& record = records[rowNumber]; record.file.existsAsFile()) {
            filePaths.add(record.file.getFullPathName());
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
        graphics.fillAll(CustomLookAndFeel::blue().withAlpha(selectedRowHighlightAlpha));
        return;
    }

    graphics.fillAll(rowNumber % 2 == 0 ? CustomLookAndFeel::greySemiDark() : CustomLookAndFeel::greyMediumDark());
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

    const auto column = tableColumnFromId(columnId);
    if (!column.has_value() || !records.containsRow(rowNumber)) {
        return;
    }

    const auto& record = records[rowNumber];
    juce::String text;
    OperationMessage operationMessage;
    std::optional<FileActivityState> activityState;
    auto justification = juce::Justification::centredLeft;
    auto textBounds
        = juce::Rectangle(layout::table::cellTextInset, 0, width - layout::table::cellTextInset * 2, height);
    auto font = CustomLookAndFeel::getTextFont().withHeight(layout::theme::tableCellFontHeight);

    switch (*column) {
        case AudioFileTableColumn::name:
            text = record.fileName;
            break;
        case AudioFileTableColumn::path:
            text = record.fullPath;
            break;
        case AudioFileTableColumn::type:
            text = formatTypeLabel(record);
            font = CustomLookAndFeel::getMonoBaseFont().withHeight(layout::theme::tableMonoCellFontHeight);
            break;
        case AudioFileTableColumn::bitDepth:
            text = formatBitsPerSampleDisplay(record);
            justification = juce::Justification::centredRight;
            break;
        case AudioFileTableColumn::bitrate:
            text = formatBitrateCompact(record);
            justification = juce::Justification::centredRight;
            break;
        case AudioFileTableColumn::sampleRate:
            text = formatSampleRateCompact(record);
            justification = juce::Justification::centredRight;
            break;
        case AudioFileTableColumn::length:
            text = formatDurationClock(record.durationSeconds);
            justification = juce::Justification::centredRight;
            break;
        case AudioFileTableColumn::peakLeft:
            text = formatPeakCompact(static_cast<double>(record.peakLeft));
            justification = juce::Justification::centredRight;
            break;
        case AudioFileTableColumn::peakRight:
            text = formatPeakCompact(static_cast<double>(record.peakRight));
            justification = juce::Justification::centredRight;
            break;
        case AudioFileTableColumn::overallPeak:
            text = formatPeakCompact(static_cast<double>(record.overallPeak));
            justification = juce::Justification::centredRight;
            break;
        case AudioFileTableColumn::overallTruePeak:
            text = formatTruePeakCompact(record.overallTruePeak);
            justification = juce::Justification::centredRight;
            break;
        case AudioFileTableColumn::maxShortTermLufs:
            text = formatLoudnessCompact(record.maxShortTermLufs);
            justification = juce::Justification::centredRight;
            break;
        case AudioFileTableColumn::integratedLufs:
            text = formatLoudnessCompact(record.integratedLufs);
            justification = juce::Justification::centredRight;
            break;
        case AudioFileTableColumn::customGain:
            text = formatCustomGainCompact(record);
            justification = juce::Justification::centredRight;
            break;
        case AudioFileTableColumn::status:
            activityState = activityStateProvider != nullptr ? activityStateProvider(record) : std::nullopt;

            if (!activityState.has_value()) {
                operationMessage = getOperationMessage(record);
                text = operationMessage.text.isNotEmpty() ? operationMessage.text : formatStatus(record);
            } else {
                text = getFileActivityLabel(activityState->activity);
                const auto indicatorSize = static_cast<float>(juce::jmin(
                    height - layout::table::activityIndicatorVerticalRoom, layout::table::activityIndicatorMaximumSize
                ));
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
                textBounds.removeFromLeft(static_cast<int>(indicatorSize) + layout::table::activityIndicatorSpacing);
            }
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
    juce::ignoreUnused(columnId);

    if (event.mods.isPopupMenu() && contextMenuRequested != nullptr) {
        contextMenuRequested(rowNumber, event);
    }
}

/// Notifies the owning component when the active sort column changes, with no column when the table is unsorted.
void AudioFileTableModel::sortOrderChanged(const int newSortColumnId, const bool isForwards)
{
    if (sortChanged != nullptr) {
        sortChanged(tableColumnFromId(newSortColumnId), isForwards);
    }
}

/// Shows the full status text for the status column of a file with an error or notice, and no tooltip for other cells.
juce::String AudioFileTableModel::getCellTooltip(const int rowNumber, const int columnId)
{
    if (tableColumnFromId(columnId) != AudioFileTableColumn::status || !records.containsRow(rowNumber)) {
        return {};
    }

    const auto& record = records[rowNumber];

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
