/// Implementation of the selected-file inspector.
/// Splits artist/title filenames above fixed analysis cards and full-width, scrollable technical and musical details,
/// or, in the wide layout, one row of cards above the short details paired on lines and the long ones below them.

#include "gui/components/AudioInfoPanel.h"

#include "analysis/AudioAnalysisFormatting.h"
#include "analysis/AudioAnalysisTypes.h"
#include "gui/components/CustomLookAndFeel.h"
#include "gui/GuiLayout.h"
#include "serato/SeratoTagTypes.h"
#include "utils/GuiUtils.h"
#include "utils/StringFormat.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

namespace audiobatch
{
namespace
{
/// Appends musical metadata, preferring Serato's analyzed tempo over standard BPM and grid fallback,
/// followed by the key, album, genre, year, and comment tags that are set.
/// The tempo, the key, and the year are short, and the album, the genre, and the comment can be long.
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
        rows.push_back({.field = "BPM", .value = utils::format("{:.2f}", *bpm), .isShort = true});
    }
    if (!data->trackInfo) {
        return;
    }
    const auto& info = *data->trackInfo;
    for (const auto& [field, value, isShort] :
         {
             std::tuple {"Key", &info.key, true},
             std::tuple {"Album", &info.album, false},
             std::tuple {"Genre", &info.genre, false},
             std::tuple {"Year", &info.year, true},
             std::tuple {"Comment", &info.comment, false},
         })
    {
        if (value->isNotEmpty()) {
            rows.push_back({.field = field, .value = *value, .isShort = isShort});
        }
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
    const auto captionFont = CustomLookAndFeel::getTextFont().withHeight(layout::information::captionFontHeight);
    utils::styleLabel(captionLabel, captionFont, CustomLookAndFeel::greyMiddleLight());
    utils::styleLabel(unitLabel, captionFont, CustomLookAndFeel::greyMiddleLight());
    utils::styleLabel(
        valueLabel,
        CustomLookAndFeel::getMonoBaseFont().withHeight(layout::information::metricFontHeight),
        CustomLookAndFeel::greySuperLight()
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

/// Keeps the same numeric font in both modes, with compact values beside stacked captions and units.
void AudioInfoMetric::setCompact(const bool compact)
{
    compactMode = compact;
    valueLabel.setFont(CustomLookAndFeel::getMonoBaseFont().withHeight(layout::information::metricFontHeight));
    valueLabel.setMinimumHorizontalScale(
        compact ? layout::compactPreview::minimumValueHorizontalScale : layout::information::minimumValueHorizontalScale
    );
    valueLabel.setJustificationType(compact ? juce::Justification::centredRight : juce::Justification::centredLeft);
    unitLabel.setJustificationType(compact ? juce::Justification::centredLeft : juce::Justification::centredRight);
    resized();
}

/// Draws a neutral card surface without implying a loudness target or clipping threshold.
void AudioInfoMetric::paint(juce::Graphics& graphics)
{
    const auto bounds = getLocalBounds().toFloat();
    graphics.setColour(CustomLookAndFeel::greyMediumDark());
    graphics.fillRoundedRectangle(bounds, layout::information::cornerRadius);
    graphics.setColour(juce::Colours::white.withAlpha(layout::information::outlineAlpha));
    graphics.drawRoundedRectangle(bounds.reduced(0.5f), layout::information::cornerRadius, 1.0f);
}

/// Stacks compact captions and units to the left of a full-height value, retaining cards in expanded mode.
void AudioInfoMetric::resized()
{
    auto bounds = getLocalBounds().reduced(layout::information::metricPadding);
    if (compactMode) {
        auto labels = bounds.removeFromLeft((bounds.getWidth() - layout::compactPreview::metricSpacing) / 2);
        labels = labels.withSizeKeepingCentre(labels.getWidth(), layout::compactPreview::metricCaptionHeight * 2);
        captionLabel.setBounds(labels.removeFromTop(layout::compactPreview::metricCaptionHeight));
        unitLabel.setBounds(labels);
        bounds.removeFromLeft(layout::compactPreview::metricSpacing);
        valueLabel.setBounds(bounds);
        return;
    }
    auto caption = bounds.removeFromTop(layout::information::metricCaptionHeight);
    unitLabel.setBounds(caption.removeFromRight(layout::information::metricUnitWidth));
    captionLabel.setBounds(caption);
    valueLabel.setBounds(bounds);
}

/// Creates the fixed summary and a non-owning viewport for secondary details.
AudioInfoPanel::AudioInfoPanel()
{
    Component::setName("Selected file information");
    const auto& textFont = CustomLookAndFeel::getTextFont();
    for (auto* label : {&filenameLabel, &titleLabel}) {
        utils::styleLabel(
            *label, textFont.withHeight(layout::information::headingFontHeight), CustomLookAndFeel::greySuperLight()
        );
    }
    utils::styleLabel(
        statusLabel, textFont.withHeight(layout::information::captionFontHeight), CustomLookAndFeel::greyMiddleLight()
    );
    utils::styleLabel(errorLabel, textFont.withHeight(layout::information::valueFontHeight), CustomLookAndFeel::red());
    errorLabel.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(filenameLabel);
    addChildComponent(titleLabel);
    // Fits the whole picture into the slot with its aspect ratio, scaling the 96 px thumbnail down to 48 px.
    artworkImage.setImagePlacement(juce::RectanglePlacement::centred);
    artworkImage.setInterceptsMouseClicks(false, false);
    artworkImage.setDescription("Embedded album artwork of the selected file");
    addChildComponent(artworkImage);
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
    statusLabel.setColour(juce::Label::textColourId, CustomLookAndFeel::greyMiddleLight());
    errorLabel.setText({}, juce::dontSendNotification);
    for (auto& metric : metrics) {
        metric.setValue("-");
    }
    detailsViewport.setViewPosition(0, 0);
    artworkImage.setImage({});
    setRows({});
}

/// Shows the album artwork beside the file name, or hides it and reclaims its space for an invalid image.
/// Only relays out this panel, so the workspace divider never moves when artwork arrives.
void AudioInfoPanel::setArtwork(const juce::Image& image)
{
    if (!image.isValid() && !hasArtwork()) {
        return;
    }
    artworkImage.setImage(image);
    resized();
    repaint();
}

/// Returns true while album artwork is shown or ready to show beside the file name.
bool AudioInfoPanel::hasArtwork() const noexcept
{
    return artworkImage.getImage().isValid();
}

/// Returns the height of the file name and title rows, raised to the artwork size while artwork is shown.
int AudioInfoPanel::getHeaderHeight() const
{
    const auto textHeight = layout::information::filenameHeight * (hasTitle ? 2 : 1);
    return hasArtwork() ? juce::jmax(textHeight, layout::information::artworkSize) : textHeight;
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
    repaint();
}

/// Switches between the expanded layout and the wide one, without changing the data or the scroll position.
void AudioInfoPanel::setWide(const bool wide)
{
    if (wideMode == wide) {
        return;
    }
    wideMode = wide;
    resized();
}

/// Format detailed audio info rows.
///
/// Channels, bit depth, sample rate, bit rate, duration, and number of samples, all of them short.
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
        .isShort = true,
    });
    newRows.push_back({
        .field = "Audio",
        .value = utils::format("{} / {}", formatSampleRateDisplay(record), formatBitrateDisplay(record)),
        .isShort = true,
    });
    newRows.push_back({
        .field = "Duration",
        .value = formatDurationClockTenths(record.durationSeconds),
        .isShort = true,
    });
    newRows.push_back({.field = "Samples", .value = juce::String(record.lengthInSamples), .isShort = true});

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
        juce::Label::textColourId, record.hasError() ? CustomLookAndFeel::red() : CustomLookAndFeel::greyMiddleLight()
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
    text.append(errorLabel.getText(), errorLabel.getFont(), CustomLookAndFeel::red());
    juce::TextLayout textLayout;
    textLayout.createLayout(text, static_cast<float>(juce::jmax(1, width)));
    return juce::roundToInt(std::ceil(textLayout.getHeight())) + layout::information::verticalPadding;
}

/// Returns the height that shows the complete summary and details without scrolling.
/// The wide layout has one row of metrics fewer, and pairs the short details on lines.
int AudioInfoPanel::getPreferredHeight() const
{
    const auto summaryHeight = wideMode
        ? layout::information::summaryHeight - layout::information::metricHeight - layout::sectionSpacing
        : layout::information::summaryHeight;
    return layout::information::verticalPadding * 2 + summaryHeight - layout::information::filenameHeight
        + getHeaderHeight() + (hasStatus ? layout::information::statusHeight : 0)
        + getDetailLineCount() * layout::information::rowHeight
        + getErrorHeight(getWidth() - layout::information::horizontalPadding * 2 - layout::scrollBarThickness);
}

/// Returns how many lines the details take: one per row,
/// or in the wide layout one per pair of short rows and one per long row.
int AudioInfoPanel::getDetailLineCount() const
{
    if (!wideMode) {
        return static_cast<int>(rows.size());
    }
    const auto shortCount = static_cast<int>(std::ranges::count_if(rows, &AudioInfoRow::isShort));
    return (shortCount + 1) / 2 + static_cast<int>(rows.size()) - shortCount;
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

/// Paints the expanded inspector surface, leaving only the individual metric cards visible in compact mode.
void AudioInfoPanel::paint(juce::Graphics& graphics)
{
    if (compactMode) {
        return;
    }
    const auto bounds = getLocalBounds().toFloat();
    graphics.setColour(CustomLookAndFeel::greySemiDark());
    graphics.fillRoundedRectangle(bounds, layout::information::cornerRadius);
    graphics.setColour(juce::Colours::white.withAlpha(layout::information::outlineAlpha));
    graphics.drawRoundedRectangle(bounds.reduced(0.5f), layout::information::cornerRadius, 1.0f);
}

/// Keeps the heading and cards outside the viewport, reserving scrollbar width to avoid relayout oscillation.
void AudioInfoPanel::resized()
{
    filenameLabel.setVisible(!compactMode);
    titleLabel.setVisible(!compactMode && hasTitle);
    artworkImage.setVisible(!compactMode && hasArtwork());
    statusLabel.setVisible(!compactMode && hasStatus);
    detailsViewport.setVisible(!compactMode);
    if (compactMode) {
        auto bounds = getLocalBounds();
        for (std::size_t index = 0; index < metrics.size(); ++index) {
            const auto remainingMetrics = static_cast<int>(metrics.size() - index);
            const auto metricWidth
                = (bounds.getWidth() - layout::compactPreview::metricSpacing * (remainingMetrics - 1))
                / remainingMetrics;
            metrics[index].setBounds(bounds.removeFromLeft(juce::jmax(0, metricWidth)));
            bounds.removeFromLeft(layout::compactPreview::metricSpacing);
        }
        return;
    }
    auto bounds
        = getLocalBounds().reduced(layout::information::horizontalPadding, layout::information::verticalPadding);
    auto header = bounds.removeFromTop(getHeaderHeight());
    if (artworkImage.isVisible()) {
        artworkImage.setBounds(
            header.removeFromLeft(layout::information::artworkSize)
                .withSizeKeepingCentre(layout::information::artworkSize, layout::information::artworkSize)
        );
        header.removeFromLeft(layout::information::artworkSpacing);
    }
    const auto textHeight = layout::information::filenameHeight * (hasTitle ? 2 : 1);
    auto text = header.withSizeKeepingCentre(header.getWidth(), textHeight);
    filenameLabel.setBounds(text.removeFromTop(layout::information::filenameHeight));
    titleLabel.setBounds(text.removeFromTop(titleLabel.isVisible() ? layout::information::filenameHeight : 0));
    statusLabel.setBounds(bounds.removeFromTop(statusLabel.isVisible() ? layout::information::statusHeight : 0));
    bounds.removeFromTop(layout::sectionSpacing);
    if (wideMode) {
        auto metricRow = bounds.removeFromTop(layout::information::metricHeight);
        for (std::size_t index = 0; index < metrics.size(); ++index) {
            const auto remainingMetrics = static_cast<int>(metrics.size() - index);
            const auto metricWidth
                = (metricRow.getWidth() - layout::sectionSpacing * (remainingMetrics - 1)) / remainingMetrics;
            metrics[index].setBounds(metricRow.removeFromLeft(juce::jmax(0, metricWidth)));
            metricRow.removeFromLeft(layout::sectionSpacing);
        }
        bounds.removeFromTop(layout::sectionSpacing);
    } else {
        for (std::size_t row = 0; row < 2; ++row) {
            auto metricRow = bounds.removeFromTop(layout::information::metricHeight);
            metrics[row * 2].setBounds(metricRow.removeFromLeft((metricRow.getWidth() - layout::sectionSpacing) / 2));
            metricRow.removeFromLeft(layout::sectionSpacing);
            metrics[row * 2 + 1].setBounds(metricRow);
            bounds.removeFromTop(layout::sectionSpacing);
        }
    }
    detailsViewport.setBounds(bounds);
    const auto contentWidth = juce::jmax(0, bounds.getWidth() - layout::scrollBarThickness);
    const auto errorHeight = getErrorHeight(contentWidth);
    errorLabel.setBounds(0, 0, contentWidth, errorHeight);
    errorLabel.setVisible(errorHeight > 0);
    const auto halfWidth = (contentWidth - layout::sectionSpacing) / 2;
    const auto shortCount = static_cast<int>(std::ranges::count_if(rows, &AudioInfoRow::isShort));
    int shortIndex = 0;
    int longLine = (shortCount + 1) / 2;
    for (std::size_t index = 0; index < rows.size(); ++index) {
        auto rowBounds = juce::Rectangle(
            0,
            errorHeight + static_cast<int>(index) * layout::information::rowHeight,
            contentWidth,
            layout::information::rowHeight
        );
        if (wideMode && rows[index].isShort) {
            const bool rightHalf = shortIndex % 2 == 1;
            rowBounds = rowBounds.withX(rightHalf ? contentWidth - halfWidth : 0)
                            .withY(errorHeight + shortIndex / 2 * layout::information::rowHeight)
                            .withWidth(halfWidth);
            ++shortIndex;
        } else if (wideMode) {
            rowBounds = rowBounds.withY(errorHeight + longLine * layout::information::rowHeight);
            ++longLine;
        }
        fieldLabels[index]->setBounds(rowBounds.removeFromLeft(layout::information::detailFieldWidth));
        valueLabels[index]->setBounds(rowBounds);
    }
    const auto detailsHeight = errorHeight + getDetailLineCount() * layout::information::rowHeight;
    detailsContent.setSize(contentWidth, juce::jmax(bounds.getHeight(), detailsHeight));
}

/// Creates accessible field/value labels only when the existing pool cannot hold the current details.
void AudioInfoPanel::ensureLabelCount(const std::size_t requiredCount)
{
    while (fieldLabels.size() < requiredCount) {
        auto fieldLabel = std::make_unique<juce::Label>();
        utils::styleLabel(
            *fieldLabel,
            CustomLookAndFeel::getTextFont().withHeight(layout::information::captionFontHeight),
            CustomLookAndFeel::greyMiddleLight()
        );
        detailsContent.addAndMakeVisible(*fieldLabel);
        fieldLabels.push_back(std::move(fieldLabel));

        auto valueLabel = std::make_unique<juce::Label>();
        utils::styleLabel(
            *valueLabel,
            CustomLookAndFeel::getTextFont().withHeight(layout::information::valueFontHeight),
            CustomLookAndFeel::greySuperLight()
        );
        valueLabel->setJustificationType(juce::Justification::centredRight);
        detailsContent.addAndMakeVisible(*valueLabel);
        valueLabels.push_back(std::move(valueLabel));
    }
}
}  // namespace audiobatch
