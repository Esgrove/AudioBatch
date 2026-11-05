/// Implementation of the selected-file inspector.
/// Splits artist/title filenames above fixed analysis cards and a two-row, five-column property grid.
/// Genre stays on one line while album, comment, and error text share exact TextLayout measurement and rendering.

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

#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
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

/// Returns the record's custom gain when it changes the signal, ignoring an about 0 dB slider residue.
std::optional<Decibels> effectiveCustomGain(const AudioAnalysisRecord& record)
{
    return record.customGain ? customGainFromDecibels(record.customGain->value) : std::nullopt;
}

/// Formats a completed hard-clip measurement or empirical limiter estimate, preserving the approximation marker.
juce::String formatAdjustedLoudness(const double loudness)
{
    return loudness <= AudioAnalysisRecord::negativeInfinityLoudness ? juce::String::fromUTF8("(≈-INF)")
                                                                     : utils::format("(≈{:.1f})", loudness);
}

/// Appends populated values without changing the original source spelling.
void appendInfoValue(std::vector<AudioInfoValue>& values, const juce::String& label, const juce::String& value)
{
    if (value.isNotEmpty()) {
        values.push_back({.label = label, .value = value});
    }
}

/// Omits unknown technical values, so they leave neither placeholders nor separators.
void appendTechnicalValue(std::vector<AudioInfoValue>& values, const juce::String& label, const juce::String& value)
{
    if (value != "-" && value != "Unknown") {
        appendInfoValue(values, label, value);
    }
}

/// Formats a known channel count without exposing an unknown zero count as metadata.
juce::String formatChannels(const int channels)
{
    if (channels == 1) {
        return "Mono";
    }
    if (channels == 2) {
        return "Stereo";
    }
    return channels > 0 ? utils::format("{} ch", channels) : juce::String();
}

/// Uses the first valid tempo from Serato analysis, the standard tag, or the final beat-grid marker.
std::optional<double> trackBpm(const serato::SeratoData& data)
{
    std::optional<double> bpm;
    if (data.autotags && std::isfinite(data.autotags->bpm) && data.autotags->bpm > 0.0) {
        bpm = data.autotags->bpm;
    } else if (
        data.trackInfo && data.trackInfo->bpm && std::isfinite(*data.trackInfo->bpm) && *data.trackInfo->bpm > 0.0
    )
    {
        bpm = data.trackInfo->bpm;
    }
    if (!bpm && data.beatGrid && !data.beatGrid->markers.empty()) {
        bpm = data.beatGrid->markers.back().bpm;
    }
    return bpm && std::isfinite(*bpm) && *bpm > 0.0 ? bpm : std::nullopt;
}

/// Recognizes a mapped Camelot input without normalizing the original spelling or its optional leading zero.
bool isCamelotInput(const juce::String& key)
{
    return key.isNotEmpty() && key[0] >= '0' && key[0] <= '9'
        && (key.endsWithIgnoreCase("A") || key.endsWithIgnoreCase("B"));
}

}  // namespace

/// Formats the three value lines and tags without any dependence on components or available width.
/// Technical details require ready analysis, but musical metadata survives pending and failed records.
AudioInfoDetails formatInfoDetails(const AudioAnalysisRecord& record, const serato::SeratoData* data)
{
    AudioInfoDetails details;
    if (record.isReady()) {
        appendTechnicalValue(details.format, "Format", formatTypeLabel(record));
        appendTechnicalValue(details.format, "Channels", formatChannels(record.channels));
        appendTechnicalValue(details.format, "Duration", formatDurationClockTenths(record.durationSeconds));
        appendTechnicalValue(details.format, "Size", formatFileSizeDisplay(record));
        if (record.bitsPerSample > 0) {
            appendInfoValue(details.audio, "Bit depth", utils::format("{}-bit", record.bitsPerSample));
        }
        appendTechnicalValue(details.audio, "Sample rate", formatSampleRateDisplay(record));
        appendTechnicalValue(details.audio, "Bitrate", formatBitrateDisplay(record));
        const auto& metrics = record.signalMetrics;
        details.dynamics = utils::format(
            "PLR {} dB · PSR {} dB · Crest {} dB",
            formatMetric(metrics.plrDb),
            formatMetric(metrics.psrDb),
            formatMetric(metrics.crestFactorDb)
        );
        details.dcOffsets = formatDcOffsets(metrics);
        details.clips = utils::format("{} total, {}", metrics.totalClips(), formatClipCounts(metrics));
        details.correlation = formatMetric(metrics.stereoCorrelation, true);
    }
    if (data == nullptr) {
        return details;
    }
    if (const auto bpm = trackBpm(*data)) {
        appendInfoValue(details.track, "BPM", utils::format("{:.2f} BPM", *bpm));
    }
    if (data->trackInfo) {
        const auto& info = *data->trackInfo;
        const auto original = info.key.trim();
        const auto camelot = camelotKey(original);
        appendInfoValue(
            details.track,
            "Key",
            camelot && !isCamelotInput(original) ? utils::format("{} / {}", original, *camelot) : original
        );
        appendInfoValue(details.track, "Year", info.year);
        details.genre = info.genre;
        details.album = info.album;
        details.comment = info.comment;
    }
    return details;
}

/// Places each property into a stable cell so missing tags never shift neighboring columns.
/// The top row holds depth, rate, bitrate, duration, and size. The bottom holds format, channels, tempo, key, and year.
std::array<AudioInfoValue, AUDIO_INFO_CELL_COUNT> formatInfoGrid(const AudioInfoDetails& details)
{
    constexpr std::array FIELDS {
        "Bit depth",
        "Sample rate",
        "Bitrate",
        "Duration",
        "Size",
        "Format",
        "Channels",
        "BPM",
        "Key",
        "Year",
    };
    std::array<AudioInfoValue, FIELDS.size()> cells;
    for (std::size_t index = 0; index < FIELDS.size(); ++index) {
        cells[index].label = FIELDS[index];
        for (const auto* values : {&details.format, &details.audio, &details.track}) {
            for (const auto& value : *values) {
                if (value.label == FIELDS[index]) {
                    cells[index].value = value.value;
                }
            }
        }
    }
    return cells;
}

/// Configures a read-only visible label with exact, shared text measurement and painting.
AudioInfoText::AudioInfoText(const juce::String& name, const bool shouldWrap) : juce::Label(name), wrapping(shouldWrap)
{
    utils::styleLabel(
        *this,
        CustomLookAndFeel::getTextFont().withHeight(layout::information::valueFontHeight),
        CustomLookAndFeel::greySuperLight()
    );
    setJustificationType(wrapping ? juce::Justification::topLeft : juce::Justification::centredLeft);
}

/// Builds the actual rendered layout, using character fallback to keep long unbroken tokens within the value lane.
/// JUCE's character wrapping still prefers word boundaries when a whole word fits.
juce::TextLayout AudioInfoText::createTextLayout(const int width) const
{
    juce::AttributedString text;
    text.setJustification(getJustificationType());
    text.setWordWrap(wrapping ? juce::AttributedString::byChar : juce::AttributedString::none);
    text.append(
        wrapping ? getText() : getText().replaceCharacters("\r\n", "  "),
        getFont(),
        findColour(juce::Label::textColourId)
    );
    juce::TextLayout textLayout;
    textLayout.createLayout(text, static_cast<float>(juce::jmax(1, width)));
    return textLayout;
}

/// Returns the height of the rendered text, with the same row clearance used during painting.
int AudioInfoText::getPreferredHeight(const int width) const
{
    if (getText().isEmpty()) {
        return 0;
    }
    if (!wrapping) {
        return layout::information::rowHeight;
    }
    return juce::jmax(
        layout::information::rowHeight,
        juce::roundToInt(std::ceil(createTextLayout(width).getHeight())) + layout::information::DETAIL_TEXT_PADDING * 2
    );
}

/// Updates the actual visible text and a labelled tooltip without adding hidden value accessors.
void AudioInfoText::setValues(const std::vector<AudioInfoValue>& values)
{
    juce::StringArray displayed;
    juce::StringArray described;
    for (const auto& value : values) {
        displayed.add(value.value);
        described.add(utils::format("{}: {}", value.label, value.value));
    }
    setText(displayed.joinIntoString(juce::String::fromUTF8(" · ")), juce::dontSendNotification);
    setDescription(described.joinIntoString(", "));
    setTooltip(getDescription());
}

/// Paints the same layout used for wrapped tag measurement, without shrinking the text.
void AudioInfoText::paint(juce::Graphics& graphics)
{
    auto bounds = getLocalBounds().toFloat();
    if (wrapping) {
        bounds.reduce(0.0f, static_cast<float>(layout::information::DETAIL_TEXT_PADDING));
    }
    createTextLayout(getWidth()).draw(graphics, bounds);
}

/// Creates accessible labels for a metric, with the unit separate from the large numeric value.
AudioInfoMetric::AudioInfoMetric(
    const juce::String& name,
    const juce::String& caption,
    const juce::String& unit,
    const juce::String& compactCaption
) :
    compactUnit(compactCaption.isNotEmpty() ? compactCaption : unit),
    measurementUnit(unit)
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
    auto description = utils::format("{}: {} {}", getName(), value, measurementUnit);
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
            "{}. After custom gain: {} {}. {}", description, adjustedValue, measurementUnit, modelDescription
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
    const auto captionWidth
        = juce::roundToInt(std::ceil(juce::TextLayout::getStringWidth(captionLabel.getFont(), captionLabel.getText())));
    const auto unitWidth
        = juce::roundToInt(std::ceil(juce::TextLayout::getStringWidth(unitLabel.getFont(), measurementUnit)));
    unitLabel.setText(measurementUnit, juce::dontSendNotification);
    if (compactMode) {
        const auto sourceWidth
            = juce::roundToInt(std::ceil(juce::TextLayout::getStringWidth(valueLabel.getFont(), valueLabel.getText())));
        const auto compactUnitWidth
            = juce::roundToInt(std::ceil(juce::TextLayout::getStringWidth(unitLabel.getFont(), compactUnit)));
        const auto preferredLabelWidth = juce::jmin(
            (bounds.getWidth() - layout::compactPreview::metricSpacing) / 2,
            juce::jmax(0, bounds.getWidth() - sourceWidth - layout::compactPreview::metricSpacing)
        );
        const auto labelWidth
            = juce::jmin(juce::jmax(0, bounds.getWidth()), juce::jmax(compactUnitWidth, preferredLabelWidth));
        auto labels = bounds.removeFromLeft(labelWidth);
        labels = labels.withSizeKeepingCentre(labels.getWidth(), layout::compactPreview::metricCaptionHeight * 2);
        const auto fullCaptionFits = labelWidth >= juce::jmax(captionWidth, unitWidth);
        captionLabel.setVisible(fullCaptionFits);
        if (fullCaptionFits) {
            captionLabel.setBounds(labels.removeFromTop(layout::compactPreview::metricCaptionHeight));
            unitLabel.setBounds(labels);
        } else {
            unitLabel.setText(compactUnit, juce::dontSendNotification);
            unitLabel.setBounds(labels);
        }
        bounds.removeFromLeft(layout::compactPreview::metricSpacing);
        layoutValues(bounds);
        return;
    }
    auto caption = bounds.removeFromTop(layout::information::metricCaptionHeight);
    const auto fullCaptionFits
        = caption.getWidth() >= captionWidth + layout::information::metricCaptionSpacing + unitWidth;
    captionLabel.setVisible(fullCaptionFits);
    if (fullCaptionFits) {
        unitLabel.setBounds(caption.removeFromRight(unitWidth));
        caption.removeFromRight(layout::information::metricCaptionSpacing);
        captionLabel.setBounds(caption);
    } else {
        unitLabel.setText(compactUnit, juce::dontSendNotification);
        unitLabel.setBounds(caption);
    }
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
    for (std::size_t index = 0; index < detailCells.size(); ++index) {
        auto& cell = detailCells[index];
        utils::styleLabel(
            cell, textFont.withHeight(layout::information::valueFontHeight), CustomLookAndFeel::greySuperLight()
        );
        cell.setJustificationType(
            index % AUDIO_INFO_COLUMN_COUNT == 0 ? juce::Justification::centredLeft : juce::Justification::centredRight
        );
        cell.setMinimumHorizontalScale(1.0f);
        detailsContent.addAndMakeVisible(cell);
    }
    for (std::size_t index = 0; index < tagCaptions.size(); ++index) {
        auto& caption = tagCaptions[index];
        utils::styleLabel(
            caption, textFont.withHeight(layout::information::captionFontHeight), CustomLookAndFeel::greyMiddleLight()
        );
        caption.setJustificationType(juce::Justification::centredLeft);
        detailsContent.addAndMakeVisible(caption);
        detailsContent.addAndMakeVisible(tagValues[index]);
    }
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
    setDetails({});
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

    if (record.isReady()) {
        const auto gain = showGainAdjustedMetrics ? effectiveCustomGain(record) : std::nullopt;
        metrics[0].setValue(
            formatPeakCompact(static_cast<double>(record.overallPeak)),
            adjustedPeak(static_cast<double>(record.overallPeak), gain)
        );
        metrics[1].setValue(formatTruePeakCompact(record.overallTruePeak), adjustedPeak(record.overallTruePeak, gain));
        sourceLoudness = {formatLoudnessCompact(record.maxShortTermLufs), formatLoudnessCompact(record.integratedLufs)};
        sourceIntegratedLufs = record.integratedLufs;
        metrics[2].setValue(sourceLoudness[0]);
        metrics[3].setValue(sourceLoudness[1]);
    }
    updateLoudnessPreview(record, showGainAdjustedMetrics);
    setDetails(formatInfoDetails(record, data));
}

/// Invalidates stale work immediately and debounces changed selection, gain, identity, or visibility policy.
/// No linear loudness estimate is displayed while waiting, since clipping and absolute gating can change it.
void AudioInfoPanel::updateLoudnessPreview(const AudioAnalysisRecord& record, const bool enabled)
{
    const auto gain = effectiveCustomGain(record);
    const bool eligible = enabled && record.isReady() && gain && record.file.existsAsFile();
    if (eligible && loudnessIdentity && loudnessGain && loudnessIdentity->path == record.file.getFullPathName()
        && loudnessIdentity->matches(record.file) && juce::approximatelyEqual(loudnessGain->value, gain->value))
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
        loudnessGain = gain;
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

/// Measures wrapped errors with the same component layout used to paint the full message.
int AudioInfoPanel::getErrorHeight(const int width) const
{
    const auto textHeight = errorLabel.getPreferredHeight(width);
    return textHeight > 0 ? textHeight + layout::information::verticalPadding : 0;
}

/// Measures all visible details at the same fixed scrollbar-reserved width used during layout.
int AudioInfoPanel::getDetailsHeight(const int width) const
{
    auto height = getErrorHeight(width);
    for (const auto visible : detailRowsVisible) {
        if (visible) {
            height += layout::information::rowHeight;
        }
    }
    const auto valueWidth = juce::jmax(0, width - layout::information::detailFieldWidth);
    for (const auto& tag : tagValues) {
        height += tag.getPreferredHeight(valueWidth);
    }
    return height;
}

/// Returns the height that shows the complete summary and exactly measured details without scrolling.
/// Only the metrics change arrangement in wide mode.
int AudioInfoPanel::getPreferredHeight() const
{
    const auto summaryHeight = wideMode
        ? layout::information::summaryHeight - layout::information::metricHeight - layout::sectionSpacing
        : layout::information::summaryHeight;
    const auto contentWidth
        = juce::jmax(0, getWidth() - layout::information::horizontalPadding * 2 - layout::scrollBarThickness);
    return layout::information::verticalPadding * 2 + summaryHeight - layout::information::filenameHeight
        + getHeaderHeight() + getDetailsHeight(contentWidth);
}

/// Updates only visible production components, preserving viewport position and the table/preview split.
void AudioInfoPanel::setDetails(const AudioInfoDetails& details)
{
    const auto cells = formatInfoGrid(details);
    detailRowsVisible.fill(false);
    for (std::size_t index = 0; index < cells.size(); ++index) {
        auto& cell = detailCells[index];
        cell.setName(cells[index].label);
        cell.setText(cells[index].value.replaceCharacters("\r\n", "  "), juce::dontSendNotification);
        const auto description = cells[index].value.isNotEmpty()
            ? utils::format("{}: {}", cells[index].label, cells[index].value)
            : juce::String();
        cell.setDescription(description);
        cell.setTooltip(description);
        cell.setVisible(cells[index].value.isNotEmpty());
        detailRowsVisible[index / AUDIO_INFO_COLUMN_COUNT]
            = detailRowsVisible[index / AUDIO_INFO_COLUMN_COUNT] || cell.isVisible();
    }
    const std::array tags {
        &details.dynamics,
        &details.dcOffsets,
        &details.clips,
        &details.correlation,
        &details.genre,
        &details.album,
        &details.comment,
    };
    for (std::size_t index = 0; index < tagValues.size(); ++index) {
        auto& value = tagValues[index];
        value.setText(*tags[index], juce::dontSendNotification);
        const auto description
            = tags[index]->isNotEmpty() ? utils::format("{}: {}", value.getName(), *tags[index]) : juce::String();
        value.setDescription(description);
        value.setTooltip(description);
        value.setVisible(tags[index]->isNotEmpty());
        tagCaptions[index].setVisible(value.isVisible());
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
    auto detailsHeight = errorHeight;
    constexpr auto COLUMN_COUNT = static_cast<int>(AUDIO_INFO_COLUMN_COUNT);
    constexpr auto COLUMN_GAP = layout::information::propertyColumnGap;
    const auto columnsWidth = juce::jmax(0, contentWidth - COLUMN_GAP * (COLUMN_COUNT - 1));
    for (std::size_t row = 0; row < detailRowsVisible.size(); ++row) {
        if (!detailRowsVisible[row]) {
            continue;
        }
        for (int column = 0; column < COLUMN_COUNT; ++column) {
            const auto left = columnsWidth * column / COLUMN_COUNT + COLUMN_GAP * column;
            const auto right = columnsWidth * (column + 1) / COLUMN_COUNT + COLUMN_GAP * column;
            detailCells[row * COLUMN_COUNT + static_cast<std::size_t>(column)].setBounds(
                left,
                detailsHeight,
                juce::jmax(0, right - left - (column == 1 ? layout::information::sampleRateColumnReduction : 0)),
                layout::information::rowHeight
            );
        }
        detailsHeight += layout::information::rowHeight;
    }
    const auto captionWidth = juce::jmin(contentWidth, layout::information::detailFieldWidth);
    const auto valueWidth = contentWidth - captionWidth;
    for (std::size_t index = 0; index < tagValues.size(); ++index) {
        auto& value = tagValues[index];
        const auto height = value.getPreferredHeight(valueWidth);
        tagCaptions[index].setBounds(
            0, detailsHeight, captionWidth, juce::jmin(height, layout::information::rowHeight)
        );
        value.setBounds(captionWidth, detailsHeight, valueWidth, height);
        detailsHeight += height;
    }
    detailsContent.setSize(contentWidth, juce::jmax(bounds.getHeight(), detailsHeight));
}
}  // namespace audiobatch
