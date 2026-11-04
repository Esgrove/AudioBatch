/// Implementation of the selected-file inspector.
/// Splits artist/title filenames above fixed analysis cards and full-width, scrollable technical and musical details,
/// or, in the wide layout, one row of cards above the short details paired on lines and the long ones below them.

#include "gui/components/AudioInfoPanel.h"

#include "analysis/AudioAnalysis.h"
#include "analysis/AudioAnalysisFormatting.h"
#include "analysis/AudioAnalysisTypes.h"
#include "gui/components/CustomLookAndFeel.h"
#include "gui/GuiLayout.h"
#include "serato/SeratoTagTypes.h"
#include "utils/GuiUtils.h"
#include "utils/Log.h"
#include "utils/StringFormat.h"
#include "utils/ThreadUtils.h"

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
/// Predicts gain-only peaks from unclamped source decibels, keeping silence and normalizing negative zero.
juce::String adjustedPeak(const double amplitude, const std::optional<Decibels> gain)
{
    if (!gain || !std::isfinite(gain->value)) {
        return {};
    }
    const auto magnitude = std::abs(amplitude);
    if (magnitude <= 0.0) {
        return "(-INF)";
    }
    const auto decibels = 20.0 * std::log10(magnitude) + static_cast<double>(gain->value);
    const auto rounded = std::round(decibels * 10.0) / 10.0;
    return utils::format("({:+.1f})", juce::approximatelyEqual(rounded, 0.0) ? 0.0 : rounded);
}

/// Formats a completed hard-clip measurement or empirical limiter estimate, preserving the approximation marker.
juce::String formatAdjustedLoudness(const double loudness)
{
    return loudness <= AudioAnalysisRecord::negativeInfinityLoudness ? juce::String::fromUTF8("(≈-INF)")
                                                                     : utils::format("(≈{:.1f})", loudness);
}

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
    adjustedValueLabel.setName(utils::format("{} after custom gain", name));
    utils::styleLabel(
        adjustedValueLabel,
        CustomLookAndFeel::getMonoBaseFont().withHeight(layout::information::adjustedMetricFontHeight),
        CustomLookAndFeel::greyMiddleLight()
    );
    unitLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(adjustedValueLabel);
    addAndMakeVisible(captionLabel);
    addAndMakeVisible(unitLabel);
    addAndMakeVisible(valueLabel);
    setValue("-");
}

/// Updates the displayed number and the complete, unit-bearing accessible description and tooltip.
void AudioInfoMetric::setValue(
    const juce::String& value,
    const juce::String& adjustedValue,
    const bool clippedLoudness,
    const bool limiterEstimate
)
{
    valueLabel.setText(value, juce::dontSendNotification);
    adjustedValueLabel.setText(adjustedValue, juce::dontSendNotification);
    adjustedValueLabel.setVisible(adjustedValue.isNotEmpty());
    auto description = utils::format("{}: {} {}", getName(), value, unitLabel.getText());
    if (adjustedValue.isNotEmpty()) {
        juce::String modelDescription = "Gain only, before clipping or plugins.";
        if (limiterEstimate) {
            modelDescription
                = "Empirical integrated limiter estimate based on measured hard clipping, calibrated to the saved "
                  "FabFilter Pro-L 2 L2 preset with a -0.3 dB ceiling. Four calibration tracks span -13.3286 to "
                  "-8.676 LUFS-I and 0 to +6 dB gain. Other source loudness is extrapolated, not validated. "
                  "Not an actual plugin render or a universal limiter prediction.";
        } else if (clippedLoudness) {
            modelDescription = "Approximate gain + hard clipping at 0 dBFS model, measured from decoded samples. "
                               "Excludes actual limiter and plugin behavior.";
        }
        description = utils::format(
            "{}. After custom gain: {} {}. {}", description, adjustedValue, unitLabel.getText(), modelDescription
        );
    }
    adjustedValueLabel.setDescription(description);
    adjustedValueLabel.setTooltip(description);
    valueLabel.setDescription(description);
    valueLabel.setTooltip(description);
    captionLabel.setTooltip(description);
    unitLabel.setTooltip(description);
    resized();
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
        const auto sourceWidth
            = juce::roundToInt(std::ceil(juce::TextLayout::getStringWidth(valueLabel.getFont(), valueLabel.getText())));
        const auto labelWidth = juce::jmin(
            (bounds.getWidth() - layout::compactPreview::metricSpacing) / 2,
            juce::jmax(0, bounds.getWidth() - sourceWidth - layout::compactPreview::metricSpacing)
        );
        auto labels = bounds.removeFromLeft(labelWidth);
        labels = labels.withSizeKeepingCentre(labels.getWidth(), layout::compactPreview::metricCaptionHeight * 2);
        captionLabel.setBounds(labels.removeFromTop(layout::compactPreview::metricCaptionHeight));
        unitLabel.setBounds(labels);
        bounds.removeFromLeft(layout::compactPreview::metricSpacing);
        layoutValues(bounds);
        return;
    }
    auto caption = bounds.removeFromTop(layout::information::metricCaptionHeight);
    unitLabel.setBounds(caption.removeFromRight(layout::information::metricUnitWidth));
    captionLabel.setBounds(caption);
    layoutValues(bounds);
}

/// Reserves the source value first and aligns the smaller prediction to its typographic baseline.
/// Narrow cards clip the prediction while the tooltip retains its complete value.
void AudioInfoMetric::layoutValues(juce::Rectangle<int> bounds)
{
    if (!adjustedValueLabel.isVisible()) {
        valueLabel.setJustificationType(
            compactMode ? juce::Justification::centredRight : juce::Justification::centredLeft
        );
        valueLabel.setBounds(bounds);
        return;
    }
    const auto sourceFont = valueLabel.getFont();
    const auto adjustedFont = adjustedValueLabel.getFont();
    const auto sourceWidth
        = juce::roundToInt(std::ceil(juce::TextLayout::getStringWidth(sourceFont, valueLabel.getText())));
    valueLabel.setJustificationType(juce::Justification::centredLeft);
    valueLabel.setBounds(bounds.removeFromLeft(juce::jmin(bounds.getWidth(), sourceWidth)));
    bounds.removeFromLeft(layout::information::adjustedMetricSpacing);
    const auto baselineOffset = juce::roundToInt(
        (sourceFont.getAscent() - sourceFont.getDescent() - adjustedFont.getAscent() + adjustedFont.getDescent()) * 0.5f
    );
    adjustedValueLabel.setBounds(bounds.translated(0, baselineOffset));
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
    utils::styleLabel(errorLabel, textFont.withHeight(layout::information::valueFontHeight), CustomLookAndFeel::red());
    errorLabel.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(filenameLabel);
    addChildComponent(titleLabel);
    // Fits the whole picture into the slot with its aspect ratio, scaling the 96 px thumbnail down to 48 px.
    artworkImage.setImagePlacement(juce::RectanglePlacement::centred);
    artworkImage.setInterceptsMouseClicks(false, false);
    artworkImage.setDescription("Embedded album artwork of the selected file");
    addChildComponent(artworkImage);
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

/// Stops timers and joins the cancellable decoder before destroying request state or child labels.
AudioInfoPanel::~AudioInfoPanel()
{
    stopTimer();
    loudnessRunner.cancel();
    loudnessRunner.waitForAllJobs();
}

/// Clears stale values while keeping a consistent empty-state summary.
void AudioInfoPanel::clear()
{
    stopTimer();
    loudnessRunner.cancel();
    loudnessIdentity.reset();
    loudnessGain.reset();
    adjustedLoudness.reset();
    hasTitle = false;
    setDescription("No file selected");
    displayedPath.clear();
    filenameLabel.setText("No file selected", juce::dontSendNotification);
    filenameLabel.setTooltip({});
    filenameLabel.setDescription({});
    titleLabel.setText({}, juce::dontSendNotification);
    titleLabel.setTooltip({});
    titleLabel.setDescription({});
    titleLabel.setVisible(false);
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
/// Omits analysis lifecycle and cache provenance, retaining error details and accessible operation context.
/// Musical metadata remains independent of analysis readiness.
/// Same-file updates preserve the detail scroll position.
void AudioInfoPanel::showRecord(
    const AudioAnalysisRecord& record,
    const juce::String& activeStatusLabel,
    const serato::SeratoData* data,
    const bool showGainAdjustedMetrics
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
    // Keep transient operation context accessible without adding a lifecycle or provenance row to the inspector.
    setDescription(activeStatusLabel.isNotEmpty() ? utils::format("{}: {}", path, activeStatusLabel) : path);
    errorLabel.setText(record.hasError() ? record.errorMessage : juce::String(), juce::dontSendNotification);
    for (auto& metric : metrics) {
        metric.setValue("-");
    }

    std::vector<AudioInfoRow> newRows;
    if (record.isReady()) {
        const auto gain = showGainAdjustedMetrics ? record.customGain : std::nullopt;
        metrics[0].setValue(
            formatPeakCompact(static_cast<double>(record.overallPeak)),
            adjustedPeak(static_cast<double>(record.overallPeak), gain)
        );
        metrics[1].setValue(formatTruePeakCompact(record.overallTruePeak), adjustedPeak(record.overallTruePeak, gain));
        sourceLoudness = {formatLoudnessCompact(record.maxShortTermLufs), formatLoudnessCompact(record.integratedLufs)};
        sourceIntegratedLufs = record.integratedLufs;
        metrics[2].setValue(sourceLoudness[0]);
        metrics[3].setValue(sourceLoudness[1]);
        newRows = formatRows(record);
    }
    updateLoudnessPreview(record, showGainAdjustedMetrics);
    appendTrackInfo(newRows, data);
    setRows(std::move(newRows));
}

/// Invalidates stale work immediately and debounces changed selection, gain, identity, or visibility policy.
/// No linear loudness estimate is displayed while waiting, since clipping and absolute gating can change it.
void AudioInfoPanel::updateLoudnessPreview(const AudioAnalysisRecord& record, const bool enabled)
{
    const bool eligible = enabled && record.isReady() && record.customGain && std::isfinite(record.customGain->value)
        && record.file.existsAsFile();
    if (eligible && loudnessIdentity && loudnessGain && loudnessIdentity->path == record.file.getFullPathName()
        && loudnessIdentity->matches(record.file)
        && juce::approximatelyEqual(loudnessGain->value, record.customGain->value))
    {
        showLoudnessPreview();
        return;
    }
    stopTimer();
    loudnessRunner.cancel();
    adjustedLoudness.reset();
    loudnessIdentity.reset();
    loudnessGain.reset();
    if (eligible) {
        loudnessIdentity.emplace(record.file);
        loudnessGain = record.customGain;
        constexpr int debounceMilliseconds {200};
        startTimer(debounceMilliseconds);
    }
}

/// Launches one latest-only measurement after gain edits settle. Delivery touches the panel only on the message thread.
void AudioInfoPanel::timerCallback()
{
    stopTimer();
    if (!loudnessIdentity || !loudnessGain) {
        return;
    }
    const auto identity = *loudnessIdentity;
    const auto gain = *loudnessGain;
    const auto runId = loudnessRunner.getCurrentRunId();
    const auto cancelled = loudnessRunner.makeCancellationCheck(runId);
    const juce::Component::SafePointer<AudioInfoPanel> safeThis(this);
    loudnessRunner.addJob([identity, gain, runId, cancelled, safeThis] {
        std::optional<GainAdjustedLoudness> result;
        try {
            result = measureGainAdjustedLoudness(juce::File(identity.path), gain, cancelled);
        } catch (...) {
            utils::logDebug("Gain-adjusted loudness preview failed: {}", utils::describeCurrentException());
        }
        if (cancelled()) {
            return;
        }
        juce::MessageManager::callAsync([identity, gain, runId, safeThis, result] {
            if (safeThis == nullptr || !safeThis->loudnessRunner.isCurrent(runId) || !safeThis->loudnessIdentity
                || !safeThis->loudnessGain || safeThis->loudnessIdentity->path != identity.path
                || !juce::approximatelyEqual(safeThis->loudnessGain->value, gain.value)
                || !identity.matches(juce::File(identity.path)))
            {
                return;
            }
            safeThis->adjustedLoudness = result;
            safeThis->showLoudnessPreview();
        });
    });
}

/// Adds only completed transient model values beside the untouched source measurements.
void AudioInfoPanel::showLoudnessPreview()
{
    metrics[2].setValue(
        sourceLoudness[0],
        adjustedLoudness ? formatAdjustedLoudness(adjustedLoudness->maxShortTermLufs) : juce::String(),
        true
    );
    const auto limiterEstimate = adjustedLoudness && loudnessGain
        ? estimateLimiterIntegratedLoudness(sourceIntegratedLufs, *loudnessGain, adjustedLoudness->integratedLufs)
        : std::nullopt;
    metrics[3].setValue(
        sourceLoudness[1],
        adjustedLoudness ? formatAdjustedLoudness(limiterEstimate.value_or(adjustedLoudness->integratedLufs))
                         : juce::String(),
        true,
        limiterEstimate.has_value()
    );
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
        + getHeaderHeight() + getDetailLineCount() * layout::information::rowHeight
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
