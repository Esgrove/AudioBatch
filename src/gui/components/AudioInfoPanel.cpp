/// Implementation of the selected-file inspector.
/// Splits artist/title filenames above fixed analysis cards and full-width, scrollable technical and musical details.

#include "gui/components/AudioInfoPanel.h"

#include "analysis/AudioAnalysisFormatting.h"
#include "gui/components/CustomLookAndFeel.h"
#include "gui/GuiLayout.h"
#include "utils/GuiUtils.h"
#include "utils/StringFormat.h"

#include <cmath>

namespace audiobatch
{
namespace
{
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
        rows.push_back({.field = "BPM", .value = utils::format("{:.2f}", *bpm)});
    }
    if (data->trackInfo && data->trackInfo->key.isNotEmpty()) {
        rows.push_back({.field = "Key", .value = data->trackInfo->key});
    }
}

}  // namespace

/// Creates accessible labels for a metric, with the unit separate from the large numeric value.
AudioInfoMetric::AudioInfoMetric(const juce::String& name, const juce::String& caption, const juce::String& unit)
{
    Component::setName(name);
    captionLabel.setText(caption, juce::dontSendNotification);
    unitLabel.setText(unit, juce::dontSendNotification);
    valueLabel.setName(name);
    const auto captionFont = juce::CustomLookAndFeel::getTextFont().withHeight(layout::information::captionFontHeight);
    utils::styleLabel(captionLabel, captionFont, juce::CustomLookAndFeel::greyMiddleLight());
    utils::styleLabel(unitLabel, captionFont, juce::CustomLookAndFeel::greyMiddleLight());
    utils::styleLabel(
        valueLabel,
        juce::CustomLookAndFeel::getMonoBaseFont().withHeight(layout::information::metricFontHeight),
        juce::CustomLookAndFeel::greySuperLight()
    );
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

/// Switches to a smaller value with its unit alongside it, leaving the full caption width available.
void AudioInfoMetric::setCompact(const bool compact)
{
    compactMode = compact;
    valueLabel.setFont(
        juce::CustomLookAndFeel::getMonoBaseFont().withHeight(
            compact ? layout::compactPreview::valueFontHeight : layout::information::metricFontHeight
        )
    );
    resized();
}

/// Draws a neutral card surface without implying a loudness target or clipping threshold.
void AudioInfoMetric::paint(juce::Graphics& graphics)
{
    const auto bounds = getLocalBounds().toFloat();
    graphics.setColour(juce::CustomLookAndFeel::greyMediumDark());
    graphics.fillRoundedRectangle(bounds, layout::information::cornerRadius);
    graphics.setColour(juce::Colours::white.withAlpha(layout::information::outlineAlpha));
    graphics.drawRoundedRectangle(bounds.reduced(0.5f), layout::information::cornerRadius, 1.0f);
}

/// Places a small caption and unit above the prominent numeric value.
void AudioInfoMetric::resized()
{
    auto bounds = getLocalBounds().reduced(layout::information::metricPadding);
    auto caption = bounds.removeFromTop(layout::information::metricCaptionHeight);
    unitLabel.setBounds(
        compactMode ? bounds.removeFromRight(layout::information::metricUnitWidth)
                    : caption.removeFromRight(layout::information::metricUnitWidth)
    );
    captionLabel.setBounds(caption);
    valueLabel.setBounds(bounds);
}

/// Creates the fixed summary and a non-owning viewport for secondary details.
AudioInfoPanel::AudioInfoPanel()
{
    Component::setName("Selected file information");
    const auto& textFont = juce::CustomLookAndFeel::getTextFont();
    for (auto* label : {&filenameLabel, &titleLabel}) {
        utils::styleLabel(
            *label,
            textFont.withHeight(layout::information::headingFontHeight),
            juce::CustomLookAndFeel::greySuperLight()
        );
    }
    utils::styleLabel(
        statusLabel,
        textFont.withHeight(layout::information::captionFontHeight),
        juce::CustomLookAndFeel::greyMiddleLight()
    );
    utils::styleLabel(
        errorLabel, textFont.withHeight(layout::information::valueFontHeight), juce::CustomLookAndFeel::red()
    );
    errorLabel.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(filenameLabel);
    addChildComponent(titleLabel);
    addAndMakeVisible(statusLabel);
    for (auto& metric : metrics) {
        addAndMakeVisible(metric);
    }
    detailsViewport.setViewedComponent(&detailsContent, false);
    detailsViewport.setScrollBarsShown(true, false);
    detailsViewport.setScrollBarThickness(layout::scrollBarThickness);
    detailsViewport.setName("File details");
    addAndMakeVisible(detailsViewport);
    detailsContent.addAndMakeVisible(errorLabel);
    clear();
}

/// Clears stale values while keeping a consistent empty-state summary.
void AudioInfoPanel::clear()
{
    hasTitle = false;
    hasStatus = false;
    setDescription("No file selected");
    displayedPath.clear();
    filenameLabel.setText("No file selected", juce::dontSendNotification);
    filenameLabel.setTooltip({});
    filenameLabel.setDescription({});
    titleLabel.setText({}, juce::dontSendNotification);
    titleLabel.setTooltip({});
    titleLabel.setDescription({});
    titleLabel.setVisible(false);
    statusLabel.setVisible(false);
    statusLabel.setColour(juce::Label::textColourId, juce::CustomLookAndFeel::greyMiddleLight());
    errorLabel.setText({}, juce::dontSendNotification);
    for (auto& metric : metrics) {
        metric.setValue("-");
    }
    detailsViewport.setViewPosition(0, 0);
    setRows({});
}

/// Reflows the existing metrics without changing their data or the details viewport's scroll position.
void AudioInfoPanel::setCompact(const bool compact)
{
    if (compactMode == compact) {
        return;
    }
    compactMode = compact;
    for (auto& metric : metrics) {
        metric.setCompact(compact);
    }
    resized();
}

/// Format detailed audio info rows.
///
/// Channels, bit depth, sample rate, bit rate, duration, number of samples, channel peaks.
std::vector<AudioInfoRow> AudioInfoPanel::formatRows(const AudioAnalysisRecord& record)
{
    std::vector<AudioInfoRow> newRows;

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
        .value = utils::format(
            "{} / {} dBFS",
            formatPeakCompact(static_cast<double>(record.peakLeft)),
            formatPeakCompact(static_cast<double>(record.peakRight))
        ),
    });

    return newRows;
}
/// Shows full-width file details and fixed metrics, clearing analysis values for pending or failed records.
/// Splits the filename at the first literal " - ", keeping subsequent separators in the title.
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
    const juce::String separator = " - ";
    const auto separatorIndex = record.fileName.indexOf(separator);
    hasTitle = separatorIndex >= 0;
    filenameLabel.setText(
        hasTitle ? record.fileName.substring(0, separatorIndex) : record.fileName, juce::dontSendNotification
    );
    titleLabel.setText(
        hasTitle ? record.fileName.substring(separatorIndex + separator.length()) : juce::String(),
        juce::dontSendNotification
    );
    for (auto* label : {&filenameLabel, &titleLabel}) {
        label->setTooltip(path);
        label->setDescription(path);
    }
    hasStatus = !record.isReady();
    const auto status = !record.isReady() && !record.hasError() && activeStatusLabel.isNotEmpty()
        ? activeStatusLabel
        : formatStatus(record);
    statusLabel.setText(status, juce::dontSendNotification);
    setDescription(utils::format("{}: {}", path, status));
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
        metrics[0].setValue(formatPeakCompact(static_cast<double>(record.overallPeak)));
        metrics[1].setValue(formatTruePeakCompact(record.overallTruePeak));
        metrics[2].setValue(formatLoudnessCompact(record.maxShortTermLufs));
        metrics[3].setValue(formatLoudnessCompact(record.integratedLufs));
        newRows = formatRows(record);
    }
    appendTrackInfo(newRows, data);
    setRows(std::move(newRows));
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
    return juce::roundToInt(std::ceil(textLayout.getHeight())) + layout::information::verticalPadding;
}

/// Returns the height that shows the complete summary and details without scrolling.
int AudioInfoPanel::getPreferredHeight() const
{
    return layout::information::verticalPadding * 2 + layout::information::summaryHeight
        + (hasTitle ? layout::information::filenameHeight : 0) + (hasStatus ? layout::information::statusHeight : 0)
        + static_cast<int>(rows.size()) * layout::information::rowHeight
        + getErrorHeight(getWidth() - layout::information::horizontalPadding * 2 - layout::scrollBarThickness);
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
            const auto& [field, value] = rows[index];
            fieldLabels[index]->setText(field, juce::dontSendNotification);
            valueLabels[index]->setName(field);
            valueLabels[index]->setText(value, juce::dontSendNotification);
            valueLabels[index]->setTooltip(utils::format("{}: {}", field, value));
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
    graphics.fillRoundedRectangle(bounds, layout::information::cornerRadius);
    graphics.setColour(juce::Colours::white.withAlpha(layout::information::outlineAlpha));
    graphics.drawRoundedRectangle(bounds.reduced(0.5f), layout::information::cornerRadius, 1.0f);
}

/// Keeps the heading and cards outside the viewport, reserving scrollbar width to avoid relayout oscillation.
void AudioInfoPanel::resized()
{
    filenameLabel.setVisible(!compactMode);
    titleLabel.setVisible(!compactMode && hasTitle);
    statusLabel.setVisible(!compactMode && hasStatus);
    detailsViewport.setVisible(!compactMode);
    if (compactMode) {
        auto bounds = getLocalBounds();
        for (auto& metric : metrics) {
            metric.setBounds(bounds.removeFromLeft(layout::compactPreview::metricWidth));
            bounds.removeFromLeft(layout::compactPreview::metricSpacing);
        }
        return;
    }
    auto bounds
        = getLocalBounds().reduced(layout::information::horizontalPadding, layout::information::verticalPadding);
    filenameLabel.setBounds(bounds.removeFromTop(layout::information::filenameHeight));
    titleLabel.setBounds(bounds.removeFromTop(titleLabel.isVisible() ? layout::information::filenameHeight : 0));
    statusLabel.setBounds(bounds.removeFromTop(statusLabel.isVisible() ? layout::information::statusHeight : 0));
    bounds.removeFromTop(layout::sectionSpacing);
    for (std::size_t row = 0; row < 2; ++row) {
        auto metricRow = bounds.removeFromTop(layout::information::metricHeight);
        metrics[row * 2].setBounds(metricRow.removeFromLeft((metricRow.getWidth() - layout::sectionSpacing) / 2));
        metricRow.removeFromLeft(layout::sectionSpacing);
        metrics[row * 2 + 1].setBounds(metricRow);
        bounds.removeFromTop(layout::sectionSpacing);
    }
    detailsViewport.setBounds(bounds);
    const auto contentWidth = juce::jmax(0, bounds.getWidth() - layout::scrollBarThickness);
    const auto errorHeight = getErrorHeight(contentWidth);
    errorLabel.setBounds(0, 0, contentWidth, errorHeight);
    errorLabel.setVisible(errorHeight > 0);
    int nextY = errorHeight;
    for (std::size_t index = 0; index < rows.size(); ++index) {
        auto rowBounds = juce::Rectangle(0, nextY, contentWidth, layout::information::rowHeight);
        fieldLabels[index]->setBounds(rowBounds.removeFromLeft(layout::information::detailFieldWidth));
        valueLabels[index]->setBounds(rowBounds);
        nextY += layout::information::rowHeight;
    }
    detailsContent.setSize(contentWidth, juce::jmax(bounds.getHeight(), nextY));
}

/// Creates accessible field/value labels only when the existing pool cannot hold the current details.
void AudioInfoPanel::ensureLabelCount(const std::size_t requiredCount)
{
    while (fieldLabels.size() < requiredCount) {
        auto fieldLabel = std::make_unique<juce::Label>();
        utils::styleLabel(
            *fieldLabel,
            juce::CustomLookAndFeel::getTextFont().withHeight(layout::information::captionFontHeight),
            juce::CustomLookAndFeel::greyMiddleLight()
        );
        detailsContent.addAndMakeVisible(*fieldLabel);
        fieldLabels.push_back(std::move(fieldLabel));

        auto valueLabel = std::make_unique<juce::Label>();
        utils::styleLabel(
            *valueLabel,
            juce::CustomLookAndFeel::getTextFont().withHeight(layout::information::valueFontHeight),
            juce::CustomLookAndFeel::greySuperLight()
        );
        valueLabel->setJustificationType(juce::Justification::centredRight);
        detailsContent.addAndMakeVisible(*valueLabel);
        valueLabels.push_back(std::move(valueLabel));
    }
}
}  // namespace audiobatch
