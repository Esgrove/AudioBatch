/// Implementation of AudioInfoPanel:
/// building the rows for an analysis record, laying out the pooled labels, and painting the panel background.

#include "gui/components/AudioInfoPanel.h"

#include "analysis/AudioAnalysis.h"
#include "analysis/AudioAnalysisFormatting.h"
#include "gui/components/CustomLookAndFeel.h"
#include "utils/StringFormat.h"

namespace audiobatch
{
namespace
{
/// Opacity of the panel background fill.
constexpr float backgroundAlpha = 0.85f;
/// Corner radius in pixels of the rounded panel background and outline.
constexpr float cornerRadius = 6.0f;
/// Opacity of the white outline drawn around the panel.
constexpr float outlineAlpha = 0.08f;
/// Opacity of the white value text in the right-hand column.
constexpr float valueTextAlpha = 0.95f;
/// Share of the panel width taken by the field name column.
constexpr float fieldColumnProportion = 0.55f;

/// Appends available musical metadata, preferring Serato's analyzed tempo over standard BPM and grid fallback.
void appendTrackInfo(std::vector<AudioInfoRow>& rows, const serato::SeratoData* data)
{
    if (data == nullptr) {
        return;
    }
    std::optional<double> bpm;
    if (data->autotags && std::isfinite(data->autotags->bpm) && data->autotags->bpm > 0.0) {
        bpm = data->autotags->bpm;
    } else if (data->trackInfo) {
        bpm = data->trackInfo->bpm;
    }
    if (!bpm && data->beatGrid && !data->beatGrid->markers.empty()) {
        bpm = data->beatGrid->markers.back().bpm;
    }
    if (bpm && std::isfinite(*bpm) && *bpm > 0.0) {
        rows.emplace_back("BPM", utils::format("{:.2f}", *bpm));
    }
    if (data->trackInfo && data->trackInfo->key.isNotEmpty()) {
        rows.emplace_back("Key", data->trackInfo->key);
    }
}
}  // namespace

/// Removes all rows so the panel shows nothing and collapses to its minimum height.
void AudioInfoPanel::clear()
{
    setRows({});
}

/// Shows the details of an analysis record.
/// A record still being analyzed shows its name and activeStatusLabel, or "Pending" when that is empty,
/// and a failed record shows its name and error.
/// Optional selected-file metadata adds BPM and key rows only when those values are available.
void AudioInfoPanel::showRecord(
    const AudioAnalysisRecord& record,
    const juce::String& activeStatusLabel,
    const serato::SeratoData* data
)
{
    std::vector<AudioInfoRow> newRows;

    if (!record.isReady() && !record.hasError()) {
        newRows.emplace_back("File", record.fileName);
        newRows.emplace_back("Status", activeStatusLabel.isNotEmpty() ? activeStatusLabel : "Pending");
        appendTrackInfo(newRows, data);
        setRows(std::move(newRows));
        return;
    }

    if (record.hasError()) {
        newRows.emplace_back("File", record.fileName);
        newRows.emplace_back("Error", record.errorMessage);
        appendTrackInfo(newRows, data);
        setRows(std::move(newRows));
        return;
    }

    newRows.emplace_back("Format", formatTypeLabel(record));
    newRows.emplace_back("Channels", juce::String(record.channels));
    newRows.emplace_back("Bits per sample", formatBitsPerSampleDisplay(record));
    newRows.emplace_back("Sample rate", juce::String(record.sampleRate));
    newRows.emplace_back("Bitrate", formatBitrateDisplay(record));
    newRows.emplace_back("Duration", juce::RelativeTime(record.durationSeconds).getDescription());
    newRows.emplace_back("Samples", juce::String(record.lengthInSamples));
    newRows.emplace_back("Peak Max", formatPeakDisplay(record.overallPeak));
    newRows.emplace_back("Peak L", formatPeakDisplay(record.peakLeft));
    newRows.emplace_back("Peak R", formatPeakDisplay(record.peakRight));
    newRows.emplace_back("True Peak Max", formatTruePeakDisplay(record.overallTruePeak));
    newRows.emplace_back("Max Short-term", formatLoudnessDisplay(record.maxShortTermLufs));
    newRows.emplace_back("Integrated Loudness", formatLoudnessDisplay(record.integratedLufs));
    appendTrackInfo(newRows, data);
    newRows.emplace_back("Status", formatStatus(record));
    setRows(std::move(newRows));
}

/// Returns the height needed to show every row plus padding, used to size the panel inside its scrolling viewport.
int AudioInfoPanel::getPreferredHeight() const
{
    const auto rowCount = static_cast<int>(rows.size());
    return verticalPadding * 2 + juce::jmax(0, rowCount) * rowHeight;
}

/// Replaces the displayed rows, reusing existing labels and hiding any leftover ones.
/// Notifies the preferred-height callback so the parent can re-run its layout.
void AudioInfoPanel::setRows(std::vector<AudioInfoRow> newRows)
{
    rows = std::move(newRows);
    ensureLabelCount(rows.size());

    for (std::size_t index = 0; index < rows.size(); ++index) {
        fieldLabels[index]->setText(rows[index].first, juce::dontSendNotification);
        valueLabels[index]->setText(rows[index].second, juce::dontSendNotification);
        fieldLabels[index]->setVisible(true);
        valueLabels[index]->setVisible(true);
    }

    for (std::size_t index = rows.size(); index < fieldLabels.size(); ++index) {
        fieldLabels[index]->setVisible(false);
        valueLabels[index]->setVisible(false);
    }

    if (onPreferredHeightChanged) {
        onPreferredHeightChanged();
    }

    resized();
    repaint();
}

/// Registers a callback invoked whenever the preferred height changes,
/// so the owner can resize the surrounding viewport.
void AudioInfoPanel::setPreferredHeightChangedCallback(std::function<void()> callback)
{
    onPreferredHeightChanged = std::move(callback);
}

/// Draws the rounded panel background with a subtle outline.
void AudioInfoPanel::paint(juce::Graphics& graphics)
{
    const auto bounds = getLocalBounds().toFloat();
    graphics.setColour(juce::CustomLookAndFeel::greySemiDark().withAlpha(backgroundAlpha));
    graphics.fillRoundedRectangle(bounds, cornerRadius);

    graphics.setColour(juce::Colours::white.withAlpha(outlineAlpha));
    graphics.drawRoundedRectangle(bounds.reduced(0.5f), cornerRadius, 1.0f);
}

/// Positions the field and value labels in two columns, one fixed-height row per entry.
void AudioInfoPanel::resized()
{
    auto bounds = getLocalBounds().reduced(horizontalPadding, verticalPadding);

    if (rows.empty()) {
        return;
    }

    const auto rowCount = static_cast<int>(rows.size());
    const auto fieldWidth = juce::roundToInt(static_cast<float>(bounds.getWidth()) * fieldColumnProportion);
    const auto fieldArea = bounds.removeFromLeft(fieldWidth);

    for (int row = 0; row < rowCount; ++row) {
        const auto fieldRow
            = juce::Rectangle(fieldArea.getX(), fieldArea.getY() + row * rowHeight, fieldArea.getWidth(), rowHeight);
        const auto valueRow
            = juce::Rectangle(bounds.getX(), bounds.getY() + row * rowHeight, bounds.getWidth(), rowHeight);

        fieldLabels[static_cast<std::size_t>(row)]->setBounds(fieldRow);
        valueLabels[static_cast<std::size_t>(row)]->setBounds(valueRow);
    }
}

/// Grows the label pools until at least the required number of field and value label pairs exist.
/// Labels are only ever added, never destroyed, so row updates stay cheap.
void AudioInfoPanel::ensureLabelCount(const std::size_t requiredCount)
{
    while (fieldLabels.size() < requiredCount) {
        auto fieldLabel = std::make_unique<juce::Label>();
        fieldLabel->setJustificationType(juce::Justification::centredLeft);
        fieldLabel->setColour(juce::Label::textColourId, juce::CustomLookAndFeel::greyMiddleLight());
        addAndMakeVisible(*fieldLabel);
        fieldLabels.push_back(std::move(fieldLabel));

        auto valueLabel = std::make_unique<juce::Label>();
        valueLabel->setJustificationType(juce::Justification::centredRight);
        valueLabel->setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(valueTextAlpha));
        addAndMakeVisible(*valueLabel);
        valueLabels.push_back(std::move(valueLabel));
    }
}
}  // namespace audiobatch
