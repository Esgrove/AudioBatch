/// Implementation of the selected-file inspector.
/// Displays fixed analysis cards and compact, scrollable technical and musical metadata without resizing the workspace.

#include "gui/components/AudioInfoPanel.h"

#include "analysis/AudioAnalysisFormatting.h"
#include "gui/components/CustomLookAndFeel.h"
#include "utils/StringFormat.h"

#include <algorithm>
#include <cmath>

namespace audiobatch
{
namespace
{
constexpr float cornerRadius = 6.0f;
constexpr float outlineAlpha = 0.08f;
constexpr float captionFontHeight = 12.0f;
constexpr float valueFontHeight = 13.0f;
constexpr float metricFontHeight = 22.0f;
constexpr float headingFontHeight = 15.0f;
constexpr int filenameHeight = 22;
constexpr int statusHeight = 18;
constexpr int sectionGap = 6;
constexpr int metricHeight = 52;
constexpr int metricPadding = 6;
constexpr int metricCaptionHeight = 16;
constexpr int metricUnitWidth = 30;
constexpr int detailFieldWidth = 60;
constexpr int compactFieldWidth = 32;
constexpr int scrollBarThickness = 8;
constexpr int summaryHeight = filenameHeight + sectionGap + metricHeight * 2 + sectionGap * 2;

/// Appends musical metadata, preferring Serato's analyzed tempo over standard BPM and grid fallback.
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
        rows.push_back({.field = "BPM", .value = utils::format("{:.2f}", *bpm), .compact = true});
    }
    if (data->trackInfo && data->trackInfo->key.isNotEmpty()) {
        rows.push_back({.field = "Key", .value = data->trackInfo->key, .compact = true});
    }
}

/// Sets a consistent, readable label style without horizontally compressing long values.
void styleLabel(juce::Label& label, const float height, const juce::Colour colour)
{
    label.setFont(juce::CustomLookAndFeel::getTextFont().withHeight(height));
    label.setColour(juce::Label::textColourId, colour);
    label.setBorderSize(juce::BorderSize<int>());
    label.setMinimumHorizontalScale(1.0f);
}
}  // namespace

/// Creates accessible labels for a metric, with the unit separate from the large numeric value.
AudioInfoMetric::AudioInfoMetric(const juce::String& name, const juce::String& caption, const juce::String& unit)
{
    Component::setName(name);
    captionLabel.setText(caption, juce::dontSendNotification);
    unitLabel.setText(unit, juce::dontSendNotification);
    valueLabel.setName(name);
    styleLabel(captionLabel, captionFontHeight, juce::CustomLookAndFeel::greyMiddleLight());
    styleLabel(unitLabel, captionFontHeight, juce::CustomLookAndFeel::greyMiddleLight());
    styleLabel(valueLabel, metricFontHeight, juce::CustomLookAndFeel::greySuperLight());
    valueLabel.setFont(juce::CustomLookAndFeel::getMonoBaseFont().withHeight(metricFontHeight));
    unitLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(captionLabel);
    addAndMakeVisible(unitLabel);
    addAndMakeVisible(valueLabel);
    setValue("-");
}

/// Updates the displayed number and the complete, unit-bearing accessible description and tooltip.
void AudioInfoMetric::setValue(const juce::String& value)
{
    valueLabel.setText(value, juce::dontSendNotification);
    const auto description = utils::format("{}: {} {}", getName(), value, unitLabel.getText());
    valueLabel.setDescription(description);
    valueLabel.setTooltip(description);
    captionLabel.setTooltip(description);
    unitLabel.setTooltip(description);
}

/// Draws a neutral card surface without implying a loudness target or clipping threshold.
void AudioInfoMetric::paint(juce::Graphics& graphics)
{
    const auto bounds = getLocalBounds().toFloat();
    graphics.setColour(juce::CustomLookAndFeel::greyMediumDark());
    graphics.fillRoundedRectangle(bounds, cornerRadius);
    graphics.setColour(juce::Colours::white.withAlpha(outlineAlpha));
    graphics.drawRoundedRectangle(bounds.reduced(0.5f), cornerRadius, 1.0f);
}

/// Places a small caption and unit above the prominent numeric value.
void AudioInfoMetric::resized()
{
    auto bounds = getLocalBounds().reduced(metricPadding);
    auto caption = bounds.removeFromTop(metricCaptionHeight);
    unitLabel.setBounds(caption.removeFromRight(metricUnitWidth));
    captionLabel.setBounds(caption);
    valueLabel.setBounds(bounds);
}

/// Creates the fixed summary and a non-owning viewport for secondary details.
AudioInfoPanel::AudioInfoPanel()
{
    Component::setName("Selected file information");
    styleLabel(filenameLabel, headingFontHeight, juce::CustomLookAndFeel::greySuperLight());
    styleLabel(statusLabel, captionFontHeight, juce::CustomLookAndFeel::greyMiddleLight());
    styleLabel(errorLabel, valueFontHeight, juce::CustomLookAndFeel::red());
    errorLabel.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(filenameLabel);
    addAndMakeVisible(statusLabel);
    for (auto& metric : metrics) {
        addAndMakeVisible(metric);
    }
    detailsViewport.setViewedComponent(&detailsContent, false);
    detailsViewport.setScrollBarsShown(true, false);
    detailsViewport.setScrollBarThickness(scrollBarThickness);
    detailsViewport.setName("File details");
    addAndMakeVisible(detailsViewport);
    detailsContent.addAndMakeVisible(errorLabel);
    clear();
}

/// Clears stale values while keeping a consistent empty-state summary.
void AudioInfoPanel::clear()
{
    displayedPath.clear();
    filenameLabel.setText("No file selected", juce::dontSendNotification);
    filenameLabel.setTooltip({});
    filenameLabel.setDescription({});
    statusLabel.setVisible(true);
    statusLabel.setText("Select a file in the list.", juce::dontSendNotification);
    statusLabel.setColour(juce::Label::textColourId, juce::CustomLookAndFeel::greyMiddleLight());
    errorLabel.setText({}, juce::dontSendNotification);
    for (auto& metric : metrics) {
        metric.setValue("-");
    }
    detailsViewport.setViewPosition(0, 0);
    setRows({});
}

/// Shows compact file details and fixed metrics, clearing analysis values for pending or failed records.
/// Ready records omit the status row to leave more room for file details.
/// Musical metadata remains independent of analysis readiness.
/// Same-file updates preserve the detail scroll position.
void AudioInfoPanel::showRecord(
    const AudioAnalysisRecord& record,
    const juce::String& activeStatusLabel,
    const serato::SeratoData* data
)
{
    const auto path = record.fullPath.isNotEmpty() ? record.fullPath : record.file.getFullPathName();
    if (displayedPath != path) {
        detailsViewport.setViewPosition(0, 0);
        displayedPath = path;
    }
    filenameLabel.setText(record.fileName, juce::dontSendNotification);
    filenameLabel.setTooltip(path);
    filenameLabel.setDescription(path);
    statusLabel.setVisible(!record.isReady());
    const auto status = !record.isReady() && !record.hasError() && activeStatusLabel.isNotEmpty()
        ? activeStatusLabel
        : formatStatus(record);
    statusLabel.setText(status, juce::dontSendNotification);
    statusLabel.setColour(
        juce::Label::textColourId,
        record.hasError() ? juce::CustomLookAndFeel::red() : juce::CustomLookAndFeel::greyMiddleLight()
    );
    errorLabel.setText(record.hasError() ? record.errorMessage : juce::String(), juce::dontSendNotification);
    for (auto& metric : metrics) {
        metric.setValue("-");
    }

    std::vector<AudioInfoRow> newRows;
    if (record.isReady()) {
        metrics[0].setValue(formatPeakCompact(record.overallPeak));
        metrics[1].setValue(formatTruePeakCompact(record.overallTruePeak));
        metrics[2].setValue(formatLoudnessCompact(record.integratedLufs));
        metrics[3].setValue(formatLoudnessCompact(record.maxShortTermLufs));
        auto channels = juce::String(record.channels);
        if (record.channels == 1) {
            channels = "Mono";
        } else if (record.channels == 2) {
            channels = "Stereo";
        }
        const auto bitDepth = formatBitsPerSampleDisplay(record);
        newRows.push_back({
            .field = "Format",
            .value = utils::format("{} / {} / {} bit", formatTypeLabel(record), channels, bitDepth),
        });
        newRows.push_back({
            .field = "Audio",
            .value = utils::format("{} / {}", formatSampleRateDisplay(record), formatBitrateDisplay(record)),
        });
        newRows.push_back({.field = "Duration", .value = juce::RelativeTime(record.durationSeconds).getDescription()});
        newRows.push_back({.field = "Samples", .value = juce::String(record.lengthInSamples)});
        newRows.push_back({
            .field = "Peak L / R",
            .value
            = utils::format("{} / {} dBFS", formatPeakCompact(record.peakLeft), formatPeakCompact(record.peakRight)),
        });
    }
    appendTrackInfo(newRows, data);
    setRows(std::move(newRows));
}

/// Counts compact musical fields as pairs so the inspector uses one row for BPM and key.
int AudioInfoPanel::getDetailsRowCount() const
{
    const auto compactCount = static_cast<int>(std::ranges::count_if(rows, &AudioInfoRow::compact));
    return static_cast<int>(rows.size()) - compactCount + (compactCount + 1) / 2;
}

/// Measures wrapped errors at the actual content width, keeping the entire message scrollable.
int AudioInfoPanel::getErrorHeight(const int width) const
{
    if (errorLabel.getText().isEmpty()) {
        return 0;
    }
    juce::AttributedString text;
    text.append(errorLabel.getText(), errorLabel.getFont(), juce::CustomLookAndFeel::red());
    juce::TextLayout textLayout;
    textLayout.createLayout(text, static_cast<float>(juce::jmax(1, width)));
    return juce::roundToInt(std::ceil(textLayout.getHeight())) + verticalPadding;
}

/// Returns the height that shows the complete summary and details without scrolling.
int AudioInfoPanel::getPreferredHeight() const
{
    return verticalPadding * 2 + summaryHeight + (statusLabel.isVisible() ? statusHeight : 0)
        + getDetailsRowCount() * rowHeight + getErrorHeight(getWidth() - horizontalPadding * 2 - scrollBarThickness);
}

/// Reuses detail labels without triggering a parent resize or resetting the table/preview split.
void AudioInfoPanel::setRows(std::vector<AudioInfoRow> newRows)
{
    rows = std::move(newRows);
    ensureLabelCount(rows.size());
    for (std::size_t index = 0; index < fieldLabels.size(); ++index) {
        const bool visible = index < rows.size();
        fieldLabels[index]->setVisible(visible);
        valueLabels[index]->setVisible(visible);
        if (visible) {
            const auto& row = rows[index];
            fieldLabels[index]->setText(row.field, juce::dontSendNotification);
            valueLabels[index]->setName(row.field);
            valueLabels[index]->setText(row.value, juce::dontSendNotification);
            valueLabels[index]->setTooltip(utils::format("{}: {}", row.field, row.value));
        }
    }
    resized();
    repaint();
}

/// Paints a subtle inspector surface behind the fixed and scrolling sections.
void AudioInfoPanel::paint(juce::Graphics& graphics)
{
    const auto bounds = getLocalBounds().toFloat();
    graphics.setColour(juce::CustomLookAndFeel::greySemiDark());
    graphics.fillRoundedRectangle(bounds, cornerRadius);
    graphics.setColour(juce::Colours::white.withAlpha(outlineAlpha));
    graphics.drawRoundedRectangle(bounds.reduced(0.5f), cornerRadius, 1.0f);
}

/// Keeps the heading and cards outside the viewport, reserving scrollbar width to avoid relayout oscillation.
void AudioInfoPanel::resized()
{
    auto bounds = getLocalBounds().reduced(horizontalPadding, verticalPadding);
    filenameLabel.setBounds(bounds.removeFromTop(filenameHeight));
    statusLabel.setBounds(bounds.removeFromTop(statusLabel.isVisible() ? statusHeight : 0));
    bounds.removeFromTop(sectionGap);
    for (std::size_t row = 0; row < 2; ++row) {
        auto metricRow = bounds.removeFromTop(metricHeight);
        metrics[row * 2].setBounds(metricRow.removeFromLeft((metricRow.getWidth() - sectionGap) / 2));
        metricRow.removeFromLeft(sectionGap);
        metrics[row * 2 + 1].setBounds(metricRow);
        bounds.removeFromTop(sectionGap);
    }
    detailsViewport.setBounds(bounds);
    const auto contentWidth = juce::jmax(0, bounds.getWidth() - scrollBarThickness);
    const auto errorHeight = getErrorHeight(contentWidth);
    errorLabel.setBounds(0, 0, contentWidth, errorHeight);
    errorLabel.setVisible(errorHeight > 0);
    int nextY = errorHeight;
    int compactColumn = 0;
    for (std::size_t index = 0; index < rows.size(); ++index) {
        const auto& row = rows[index];
        const auto width = row.compact ? (contentWidth - sectionGap) / 2 : contentWidth;
        auto rowBounds = juce::Rectangle(compactColumn * (width + sectionGap), nextY, width, rowHeight);
        fieldLabels[index]->setBounds(rowBounds.removeFromLeft(row.compact ? compactFieldWidth : detailFieldWidth));
        valueLabels[index]->setBounds(rowBounds);
        compactColumn = row.compact ? 1 - compactColumn : 0;
        if (compactColumn == 0) {
            nextY += rowHeight;
        }
    }
    if (compactColumn != 0) {
        nextY += rowHeight;
    }
    detailsContent.setSize(contentWidth, juce::jmax(bounds.getHeight(), nextY));
}

/// Creates accessible field/value labels only when the existing pool cannot hold the current details.
void AudioInfoPanel::ensureLabelCount(const std::size_t requiredCount)
{
    while (fieldLabels.size() < requiredCount) {
        auto fieldLabel = std::make_unique<juce::Label>();
        styleLabel(*fieldLabel, captionFontHeight, juce::CustomLookAndFeel::greyMiddleLight());
        detailsContent.addAndMakeVisible(*fieldLabel);
        fieldLabels.push_back(std::move(fieldLabel));

        auto valueLabel = std::make_unique<juce::Label>();
        styleLabel(*valueLabel, valueFontHeight, juce::CustomLookAndFeel::greySuperLight());
        valueLabel->setJustificationType(juce::Justification::centredRight);
        detailsContent.addAndMakeVisible(*valueLabel);
        valueLabels.push_back(std::move(valueLabel));
    }
}
}  // namespace audiobatch
